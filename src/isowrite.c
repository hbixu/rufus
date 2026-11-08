/*
 * Rufus: The Reliable USB Formatting Utility
 * Windows ISO file output
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <ctype.h>

#include "rufus.h"
#include "wue.h"
#include "drive.h"
#include "wimlib.h"
#include "missing.h"
#include "resource.h"
#include "settings.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "isowrite.h"

// Workspace layout: <temp_dir>\RufusISO-XXXXXXXX\iso (mapped to a drive letter) + bootorder.txt
#define ISO_WORKDIR_PREFIX          "RufusISO-"
#define ISO_TREE_DIR                "iso"
#define ISO_BOOT_ORDER_FILE         "bootorder.txt"
#define ISO_MAX_LABEL_LENGTH        32
#define OSCDIMG_PROGRESS_PATTERN    ".*\r([0-9\\.]+)% complete.*"

extern BOOL preserve_timestamps, enable_ntfs_compression, validate_md5sum;
extern int unattend_xml_flags;
extern char *unattend_xml_path, *archive_path;

static char oscdimg_path[MAX_PATH];

// temp_dir normally ends with a backslash, except when it falls back to the current directory
static const char* TempDirSeparator(void)
{
	size_t len = strlen(temp_dir);
	return ((len != 0) && (temp_dir[len - 1] == '\\')) ? "" : "\\";
}

static const char* GetOscdimgPath(void)
{
	static_sprintf(oscdimg_path, "%s\\%s\\oscdimg_%s.exe", app_data_dir, FILES_DIR, APPLICATION_ARCH);
	return oscdimg_path;
}

int EnsureOscdimg(void)
{
	const char* path = GetOscdimgPath();

	if (PathFileExistsU((char*)path)) {
		if (FileMatchesHash(path, OSCDIMG_HASH))
			return 1;
		// Don't wait until after the ISO has been extracted to find out that oscdimg can't be used
		uprintf("WARNING: Existing 'oscdimg.exe' hash does not match expected value - Deleting it");
		DeleteFileU(path);
	}
	if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_115), lmprintf(MSG_337, "oscdimg.exe")) != IDYES)
		return 0;
	IGNORE_RETVAL(_chdirU(app_data_dir));
	IGNORE_RETVAL(_mkdir(FILES_DIR));
	if ((DownloadToFileOrBufferEx(OSCDIMG_URL, path, SYMBOL_SERVER_USER_AGENT, NULL, hMainDialog, FALSE) < 64 * KB) ||
		!FileMatchesHash(path, OSCDIMG_HASH)) {
		uprintf("Could not download a valid 'oscdimg.exe'");
		return -1;
	}
	return 1;
}

// Keep only characters that are safe for an ISO/UDF volume label on the oscdimg command line
static void SanitizeIsoLabel(const char* src, char* dst, size_t dst_size)
{
	size_t i, j;

	for (i = 0, j = 0; (src[i] != 0) && (j < dst_size - 1) && (j < ISO_MAX_LABEL_LENGTH); i++) {
		unsigned char c = (unsigned char)src[i];
		if ((c & 0x80) != 0) {
			// Skip UTF-8 continuation bytes and replace the whole codepoint with a single '_'
			if ((c & 0xC0) == 0x80)
				continue;
			dst[j++] = '_';
		} else {
			dst[j++] = (isalnum(c) || c == '_' || c == '-' || c == '.') ? (char)c : '_';
		}
	}
	dst[j] = 0;
}

char* GetIsoOutputPath(void)
{
	static char filename[ISO_MAX_LABEL_LENGTH + 16];
	char label[ISO_MAX_LABEL_LENGTH + 1], *iso_path, *dir, *p;
	EXT_DECL(iso_ext, filename, __VA_GROUP__("*.iso"), __VA_GROUP__(lmprintf(MSG_036)));

	SanitizeIsoLabel((img_report.label[0] != 0) ? img_report.label : APPLICATION_NAME, label, sizeof(label));
	static_sprintf(filename, "%s_Rufus.iso", label);
	dir = ReadSettingStr(SETTING_ISO_OUTPUT_DIR);
	iso_path = FileDialog(TRUE, (dir != NULL && dir[0] != 0 && PathFileExistsU(dir)) ? dir : NULL, &iso_ext, NULL);
	if (iso_path == NULL)
		return NULL;

	// Never allow the source image to be overwritten
	if ((image_path != NULL) && (_stricmp(iso_path, image_path) == 0)) {
		uprintf("The destination ISO cannot be the source image");
		Notification(MB_OK | MB_ICONERROR, APPLICATION_NAME, lmprintf(MSG_382));
		free(iso_path);
		return NULL;
	}

	// Remember the destination directory
	dir = _strdup(iso_path);
	if (dir != NULL) {
		p = strrchr(dir, '\\');
		if (p != NULL) {
			*p = 0;
			WriteSettingStr(SETTING_ISO_OUTPUT_DIR, dir);
		}
		free(dir);
	}
	return iso_path;
}

// Return the free space of the volume holding 'path' and the volume's root in 'volume'
static uint64_t GetVolumeFreeSpace(const char* path, char* volume, int volume_size)
{
	wchar_t wvolume[MAX_PATH];
	ULARGE_INTEGER free_bytes = { 0 };
	wconvert(path);

	volume[0] = 0;
	if ((wpath == NULL) || !GetVolumePathNameW(wpath, wvolume, ARRAYSIZE(wvolume)) ||
		!GetDiskFreeSpaceExW(wvolume, &free_bytes, NULL, NULL)) {
		wfree(path);
		return 0;
	}
	wchar_to_utf8_no_alloc(wvolume, volume, volume_size);
	wfree(path);
	return free_bytes.QuadPart;
}

static void ReportNotEnoughSpace(uint64_t required, const char* volume, uint64_t available)
{
	char str_required[32], str_available[32];

	static_strcpy(str_required, SizeToHumanReadable(required, FALSE, FALSE));
	static_strcpy(str_available, SizeToHumanReadable(available, FALSE, FALSE));
	uprintf("Not enough space on '%s': %s required, %s available", volume, str_required, str_available);
	Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_376), lmprintf(MSG_377, str_required, volume, str_available));
}

BOOL CheckIsoOutputSpace(const char* iso_path)
{
	char tmp_volume[MAX_PATH], iso_volume[MAX_PATH];
	uint64_t required, tmp_free, iso_free;

	// The workspace holds the extracted ISO, and the destination receives an ISO of about the same size.
	// Add the (uncompressed) archive files, estimated at twice the archive size, plus a 5% + 64 MB margin.
	required = _filesizeU(image_path);
	if (archive_path != NULL)
		required += 2 * _filesizeU(archive_path);
	required += required / 20 + 64 * MB;

	tmp_free = GetVolumeFreeSpace(temp_dir, tmp_volume, sizeof(tmp_volume));
	iso_free = GetVolumeFreeSpace(iso_path, iso_volume, sizeof(iso_volume));
	if ((tmp_free == 0) || (iso_free == 0)) {
		uprintf("WARNING: Could not determine the free space for ISO creation - Proceeding anyway");
		return TRUE;
	}
	if (_stricmp(tmp_volume, iso_volume) == 0) {
		if (tmp_free < 2 * required) {
			ReportNotEnoughSpace(2 * required, tmp_volume, tmp_free);
			return FALSE;
		}
	} else {
		if (tmp_free < required) {
			ReportNotEnoughSpace(required, tmp_volume, tmp_free);
			return FALSE;
		}
		if (iso_free < required) {
			ReportNotEnoughSpace(required, iso_volume, iso_free);
			return FALSE;
		}
	}
	return TRUE;
}

// Map (or unmap) a drive letter to a directory, as 'subst' does
static BOOL MapDriveLetter(const char* drive_name, const char* target, BOOL map)
{
	BOOL r;
	wconvert(drive_name);
	wconvert(target);

	// Same as AltUnmountVolume(): removing without a target removes the most recent definition
	r = DefineDosDeviceW(map ? DDD_NO_BROADCAST_SYSTEM : (DDD_REMOVE_DEFINITION | DDD_NO_BROADCAST_SYSTEM),
		wdrive_name, map ? wtarget : NULL);
	if (!r)
		uprintf("Could not %s '%s' %s '%s': %s", map ? "map" : "unmap", drive_name, map ? "to" : "from",
			target, WindowsErrorString());
	wfree(drive_name);
	wfree(target);
	return r;
}

// Extract the 'Windows UEFI CA 2023' El Torito UEFI boot image from boot.wim into the ISO tree
static BOOL ExtractEfisysEx(char drive_letter)
{
	const char* src[] = {
		"Windows\\Boot\\DVD_EX\\EFI\\en-US\\efisys_EX.bin",
		"Windows\\Boot\\DVD_EX\\EFI\\*\\efisys_EX.bin"
	};
	char boot_wim[] = "?:\\sources\\boot.wim", dst_dir[] = "?:\\efi\\microsoft\\boot";
	char dst_file[] = "?:\\efi\\microsoft\\boot\\efisys_EX.bin";
	int i, index = 2;
	BOOL r = FALSE;
	WIMStruct* wim = NULL;

	boot_wim[0] = drive_letter;
	dst_dir[0] = drive_letter;
	dst_file[0] = drive_letter;
	wimlib_global_init(0);
	if (wimlib_open_wimU(boot_wim, 0, &wim) != 0) {
		uprintf("Could not open '%s'", boot_wim);
		goto out;
	}
	// Setup image is index 2 on official media (same logic as ApplyWindowsCustomization())
	if (wimlib_resolve_image(wim, L"2") != 2)
		index = 1;
	for (i = 0; (i < ARRAYSIZE(src)) && !r; i++) {
		if (wimlib_extract_pathsU(wim, index, dst_dir, &src[i], 1, WIMLIB_EXTRACT_FLAG_NO_ACLS |
			WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE | ((i == 0) ? 0 : WIMLIB_EXTRACT_FLAG_GLOB_PATHS)) == 0)
			r = PathFileExistsU(dst_file);
	}
	if (r)
		uprintf("Extracted '%s' from '%s[%d]'", dst_file, boot_wim, index);

out:
	if (wim != NULL)
		wimlib_free(wim);
	wimlib_global_cleanup();
	return r;
}

// Create the oscdimg boot order file, so that the boot files are located at the beginning of the image.
// Microsoft's oscdimg documentation states that this is required for images larger than 4.5 GB.
static BOOL CreateBootOrderFile(char drive_letter, const char* order_path)
{
	const char* dirs[] = { "boot", "efi" };
	const char* files[] = { "bootmgr", "bootmgr.efi", "sources\\boot.wim" };
	char path[MAX_PATH];
	uint32_t i, j, n = 0;
	StrArray list;
	FILE* fd;

	fd = fopenU(order_path, "wb");
	if (fd == NULL) {
		uprintf("Could not create '%s'", order_path);
		return FALSE;
	}
	StrArrayCreate(&list, 128);
	for (i = 0; i < ARRAYSIZE(dirs); i++) {
		static_sprintf(path, "%c:\\%s", drive_letter, dirs[i]);
		StrArrayClear(&list);
		ListDirectoryContent(&list, path, LIST_DIR_TYPE_FILE | LIST_DIR_TYPE_RECURSIVE);
		for (j = 0; j < list.Index; j++) {
			// Strip the "X:\" prefix
			if (strlen(list.String[j]) > 3) {
				fprintf(fd, "%s\r\n", &list.String[j][3]);
				n++;
			}
		}
	}
	for (i = 0; i < ARRAYSIZE(files); i++) {
		static_sprintf(path, "%c:\\%s", drive_letter, files[i]);
		if (PathFileExistsU(path)) {
			fprintf(fd, "%s\r\n", files[i]);
			n++;
		}
	}
	StrArrayDestroy(&list);
	fclose(fd);
	uprintf("Created boot order file with %d entries", n);
	return (n != 0);
}

// oscdimg's documentation says not to use spaces in its arguments, so try to use a short path
static void GetOscdimgArgPath(const char* path, char* dst, size_t dst_size)
{
	wchar_t wshort[MAX_PATH];
	char short_path[MAX_PATH];
	wconvert(path);

	if ((wpath != NULL) && (GetShortPathNameW(wpath, wshort, ARRAYSIZE(wshort)) != 0) &&
		(wchar_to_utf8_no_alloc(wshort, short_path, sizeof(short_path)) != 0) && (strchr(short_path, ' ') == NULL))
		safe_sprintf(dst, dst_size, "%s", short_path);
	else
		safe_sprintf(dst, dst_size, "\"%s\"", path);
	wfree(path);
}

DWORD WINAPI IsoOutputThread(void* param)
{
	char* iso_path = (char*)param;
	char work_dir[MAX_PATH] = "", tree_dir[MAX_PATH], order_path[MAX_PATH], order_arg[MAX_PATH + 2];
	char drive_name[] = "?:", label[ISO_MAX_LABEL_LENGTH + 1], tmp[128], efisys_name[32] = "efisys.bin";
	char cmd[4 * KB], bios_entry[64] = "", uefi_entry[96] = "", efisys_path[MAX_PATH];
	int i, nb_entries = 0, saved_fs_type = fs_type;
	BOOL mapped = FALSE, success = FALSE, iso_written = FALSE, saved_validate_md5sum = validate_md5sum;
	BOOL saved_preserve_timestamps = preserve_timestamps, saved_enable_ntfs_compression = enable_ntfs_compression;
	DWORD r;
	HANDLE exe = INVALID_HANDLE_VALUE;

	if_assert_fails((iso_path != NULL) && (image_path != NULL))
		goto out;

	uprintf("Creating bootable ISO file '%s' from '%s'", iso_path, image_path);
	PrintInfo(0, MSG_378);
	UpdateProgress(OP_FILE_COPY, 0.0f);

	// Settings that only apply to drives (or that would alter the ISO content) are disabled
	// for the duration of the operation, and restored on exit.
	fs_type = FS_NTFS;					// Prevents splitting of a >4GB install.wim, which only applies to FAT
	validate_md5sum = FALSE;			// Runtime UEFI validation replaces the bootloaders
	enable_ntfs_compression = FALSE;	// Would run 'compact' on the extracted files
	preserve_timestamps = TRUE;			// Keep the timestamps of the source ISO

	// Create the workspace
	for (i = 0; i < 16; i++) {
		static_sprintf(work_dir, "%s%s%s%08X", temp_dir, TempDirSeparator(), ISO_WORKDIR_PREFIX, GetTickCount() + i);
		if (CreateDirectoryU(work_dir, NULL))
			break;
		work_dir[0] = 0;
	}
	if (work_dir[0] == 0) {
		uprintf("Could not create a workspace in '%s': %s", temp_dir, WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_CANNOT_MAKE);
		goto out;
	}
	static_sprintf(tree_dir, "%s\\%s", work_dir, ISO_TREE_DIR);
	static_sprintf(order_path, "%s\\%s", work_dir, ISO_BOOT_ORDER_FILE);
	if (!CreateDirectoryU(tree_dir, NULL)) {
		uprintf("Could not create '%s': %s", tree_dir, WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_CANNOT_MAKE);
		goto out;
	}

	// Map the ISO tree to a drive letter, so that the existing extraction and customization
	// code, that expects a 'X:' destination, can be used without modifications.
	drive_name[0] = GetUnusedDriveLetter();
	if (drive_name[0] == 0) {
		uprintf("Could not find an unused drive letter");
		ErrorStatus = RUFUS_ERROR(ERROR_NO_MORE_ITEMS);
		goto out;
	}
	if (!MapDriveLetter(drive_name, tree_dir, TRUE)) {
		ErrorStatus = RUFUS_ERROR(GetLastError());
		goto out;
	}
	mapped = TRUE;
	uprintf("Using workspace '%s' mapped as %s", tree_dir, drive_name);
	CHECK_FOR_USER_CANCEL;

	// Extract the ISO
	if (!ExtractISO(image_path, drive_name, FALSE)) {
		if (!IS_ERROR(ErrorStatus))
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_EXTRACT));
		goto out;
	}
	CHECK_FOR_USER_CANCEL;

	// Apply the Windows customization
	if (unattend_xml_path != NULL) {
		if (!ApplyWindowsCustomization(drive_name[0], unattend_xml_flags)) {
			if (!IS_ERROR(ErrorStatus))
				ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_PATCH));
			goto out;
		}
		// The UEFI boot of an ISO uses the El Torito boot image, so the 2023 bootloaders that
		// ApplyWindowsCustomization() copied to the tree need the matching boot image as well.
		if (unattend_xml_flags & UNATTEND_USE_MS2023_BOOTLOADERS) {
			if (ExtractEfisysEx(drive_name[0])) {
				static_strcpy(efisys_name, "efisys_EX.bin");
			} else {
				uprintf("WARNING: 'efisys_EX.bin' was not found - The UEFI boot image will not be 'Windows UEFI CA 2023' signed");
				Notification(MB_OK | MB_ICONWARNING, APPLICATION_NAME, lmprintf(MSG_381));
			}
		}
	}
	CHECK_FOR_USER_CANCEL;

	// Copy any additional files from an optional archive selected by the user
	if (archive_path != NULL) {
		UpdateProgress(OP_EXTRACT_ZIP, 0.0f);
		if (!ExtractZip(archive_path, drive_name) && !IS_ERROR(ErrorStatus))
			uprintf("WARNING: Could not copy additional files");
	}
	CHECK_FOR_USER_CANCEL;

	// Set the El Torito boot entries from the boot images present in the tree
	static_sprintf(efisys_path, "%s\\efi\\microsoft\\boot\\%s", drive_name, efisys_name);
	static_sprintf(tmp, "%s\\boot\\etfsboot.com", drive_name);
	if (img_report.has_etfsboot && PathFileExistsU(tmp)) {
		static_sprintf(bios_entry, "#p0,e,b%s", tmp);
		nb_entries++;
	}
	if (img_report.has_efisys && PathFileExistsU(efisys_path)) {
		static_sprintf(uefi_entry, "#pEF,e,b%s", efisys_path);
		nb_entries++;
	}
	if (nb_entries == 0) {
		uprintf("No El Torito boot image found - Cannot create a bootable ISO");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_EXTRACT));
		goto out;
	}
	if (!CreateBootOrderFile(drive_name[0], order_path)) {
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	GetOscdimgArgPath(order_path, order_arg, sizeof(order_arg));

	// The volume label (from the UI, which defaults to the source ISO label)
	GetWindowTextU(hLabel, tmp, sizeof(tmp));
	SanitizeIsoLabel((tmp[0] != 0) ? tmp : img_report.label, label, sizeof(label));
	if (label[0] == 0)
		static_strcpy(label, APPLICATION_NAME);

	// Lock oscdimg.exe and validate that it has not been tampered with
	GetOscdimgPath();
	exe = CreateFileU(oscdimg_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (exe == INVALID_HANDLE_VALUE) {
		uprintf("ERROR: Could not lock 'oscdimg.exe'");
		ErrorStatus = RUFUS_ERROR(GetLastError());
		goto out;
	}
	if (!FileMatchesHash(oscdimg_path, OSCDIMG_HASH)) {
		uprintf("ERROR: Existing 'oscdimg.exe' hash does not match expected value!");
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_IMAGE_HASH);
		goto out;
	}

	// Create the ISO. Same file system options as Microsoft's media: UDF 1.02 only, since Microsoft's
	// ISO-9660 implementation doesn't support files larger than 4 GB, with BIOS and UEFI El Torito entries.
	UpdateProgress(OP_CREATE_ISO, 0.0f);
	static_sprintf(cmd, "\"%s\" -m -h -g -u2 -udfver102 -l\"%s\" -yo%s -bootdata:%d%s%s %s\\ \"%s\"",
		oscdimg_path, label, order_arg, nb_entries, bios_entry, uefi_entry, drive_name, iso_path);
	if (strlen(cmd) >= sizeof(cmd) - 1) {
		uprintf("ERROR: The oscdimg command line is too long");
		ErrorStatus = RUFUS_ERROR(ERROR_FILENAME_EXCED_RANGE);
		goto out;
	}
	uprintf("Running command: '%s'", cmd);
	iso_written = TRUE;
	r = RunCommandWithProgressEx(cmd, sysnative_dir, FALSE, OP_CREATE_ISO, MSG_379, OSCDIMG_PROGRESS_PATTERN);
	if (r != 0) {
		if (!IS_ERROR(ErrorStatus)) {
			SetLastError(r);
			uprintf("Failed to create ISO image: %s", WindowsErrorString());
			ErrorStatus = RUFUS_ERROR(SCODE_CODE(r));
		}
		goto out;
	}
	success = !IS_ERROR(ErrorStatus);

out:
	safe_closehandle(exe);
	if (mapped)
		MapDriveLetter(drive_name, tree_dir, FALSE);
	if (work_dir[0] != 0) {
		uprintf("Deleting workspace '%s'...", work_dir);
		SHDeleteDirectoryExU(NULL, work_dir, FOF_NO_UI);
	}
	if (success) {
		uprintf("Created '%s' (%s)", iso_path, SizeToHumanReadable(_filesizeU(iso_path), FALSE, FALSE));
		uprintf("%s", lmprintf(MSG_380, iso_path));
	} else if (iso_written && PathFileExistsU(iso_path)) {
		// Don't leave a partial ISO behind (but never delete a file that oscdimg didn't write)
		uprintf("Deleting incomplete '%s'", iso_path);
		DeleteFileU(iso_path);
	}
	fs_type = saved_fs_type;
	validate_md5sum = saved_validate_md5sum;
	preserve_timestamps = saved_preserve_timestamps;
	enable_ntfs_compression = saved_enable_ntfs_compression;
	safe_free(iso_path);
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	ExitThread(0);
}

void CleanupIsoOutputLeftovers(void)
{
	char pattern[MAX_PATH], name[MAX_PATH], dir[MAX_PATH], tree[MAX_PATH], target[MAX_PATH + 8];
	char drive_name[] = "?:";
	wchar_t wdrive[] = L"?:", wtarget[MAX_PATH + 8];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	wchar_t* wpattern;
	size_t i;
	char c;

	static_sprintf(pattern, "%s%s%s*", temp_dir, TempDirSeparator(), ISO_WORKDIR_PREFIX);
	wpattern = utf8_to_wchar(pattern);
	if (wpattern == NULL)
		return;
	h = FindFirstFileW(wpattern, &fd);
	free(wpattern);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		if (wchar_to_utf8_no_alloc(fd.cFileName, name, sizeof(name)) == 0)
			continue;
		// Only process directories that match our exact naming pattern: prefix + 8 hex digits
		if (strlen(name) != strlen(ISO_WORKDIR_PREFIX) + 8)
			continue;
		for (i = strlen(ISO_WORKDIR_PREFIX); (name[i] != 0) && isxdigit((unsigned char)name[i]); i++);
		if (name[i] != 0)
			continue;
		static_sprintf(dir, "%s%s%s", temp_dir, TempDirSeparator(), name);
		static_sprintf(tree, "%s\\%s", dir, ISO_TREE_DIR);
		// ...and that have our ISO tree subdirectory
		if (!PathFileExistsU(tree))
			continue;
		// Remove any drive letter that still maps to the workspace (QueryDosDevice reports '\??\<path>')
		static_sprintf(target, "\\??\\%s", tree);
		for (c = 'A'; c <= 'Z'; c++) {
			wdrive[0] = (wchar_t)c;
			if (QueryDosDeviceW(wdrive, wtarget, ARRAYSIZE(wtarget)) == 0)
				continue;
			wchar_to_utf8_no_alloc(wtarget, name, sizeof(name));
			if (_stricmp(name, target) == 0) {
				drive_name[0] = c;
				uprintf("Removing leftover drive mapping %s → '%s'", drive_name, tree);
				MapDriveLetter(drive_name, tree, FALSE);
			}
		}
		uprintf("Removing leftover ISO workspace '%s'", dir);
		SHDeleteDirectoryExU(NULL, dir, FOF_NO_UI);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}
