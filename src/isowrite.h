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

#include <windows.h>

#pragma once

/*
 * Writing a (customized) Windows installation ISO to a bootable ISO file:
 * - The ISO content is extracted into a temporary workspace that is mapped to a drive letter,
 *   so that the existing ExtractISO() and ApplyWindowsCustomization() code can be reused as is.
 * - The bootable ISO is then created with Microsoft's oscdimg, using the El Torito BIOS
 *   (etfsboot.com) and UEFI (efisys.bin or efisys_EX.bin) boot images of the source media.
 * See docs/iso-output for the analysis and design.
 */

// Ensure that a valid oscdimg.exe is available, downloading it (after user confirmation) if needed.
// Can be called from a worker thread. Returns 1 if available, 0 if the user declined the download
// and -1 if the download failed.
extern int EnsureOscdimg(void);
// Prompt the user for the destination ISO file. Returns an allocated path or NULL.
extern char* GetIsoOutputPath(void);
// Check that there is enough free space for the workspace and the destination ISO.
// Displays an error notification and returns FALSE if there isn't.
extern BOOL CheckIsoOutputSpace(const char* iso_path);
// Thread that creates the ISO file. 'param' is the destination path, allocated with malloc
// and freed by the thread. Posts UM_FORMAT_COMPLETED when done.
extern DWORD WINAPI IsoOutputThread(void* param);
// Remove workspaces and drive letter mappings left over by a previous instance that was
// terminated while creating an ISO.
extern void CleanupIsoOutputLeftovers(void);
