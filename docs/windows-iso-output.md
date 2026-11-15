# Write to a bootable ISO file (Windows sources)

Feature of the internal Rufus fork (`hbixu/rufus`, branch `develop`, commit `1c45650d`, based on Rufus 4.16). Instead of writing to a USB drive, Rufus creates a bootable ISO file with the Windows customization already applied.

**Status (2026-11-15):** implemented and working with Windows ISOs. Boot tests passed in VirtualBox (UEFI with Secure Boot, and BIOS) and through Ventoy on a physical PC.

## 1. Usage

1. Select a Windows installation ISO.
2. Under "Show advanced drive properties", tick **"Write to a bootable ISO file"**. The checkbox is only enabled for eligible Windows ISOs (section 3.1); its tooltip explains why when it is disabled.
3. Optionally change the label (defaults to the source ISO label).
4. Start → choose the customization options → choose the destination file (suggested name `<label>_Rufus.iso`; the last folder used is remembered).

No USB drive needs to be plugged in. With the checkbox ticked, the device list, partition scheme, target system, file system, cluster size, quick format, bad blocks and related options are disabled. Hiding the advanced section turns ISO mode off.

## 2. Decisions

| Topic | Decision |
|---|---|
| Scope | Windows ISOs only |
| Writing to USB | No behaviour change. Everything depends on the global `iso_output`, false by default and not persisted |
| ISO generator | Microsoft's oscdimg (the same tool Rufus already uses for "save as ISO"), with BIOS and UEFI El Torito entries |
| Workspace | Temporary folder mapped to a drive letter with `DefineDosDevice`, so that `ExtractISO` and `ApplyWindowsCustomization` are reused unchanged |
| External dependencies | oscdimg downloaded at runtime, after user confirmation, validated by SHA-256 |
| Silent install | **Excluded** in ISO mode (section 4.1) |
| CA 2023 bootloaders | Kept in ISO mode (section 4.2) |
| Missing `efisys_EX.bin` | Falls back to `efisys.bin` and warns the user (MSG_381) |
| Checkbox location | "Advanced drive properties" section |
| New code | Kept in `src/isowrite.c` / `isowrite.h`; existing files only get short decision points, to ease syncing with upstream Rufus |

## 3. How it works

### 3.1 Eligibility

Macro `IS_ISO_OUTPUT_ELIGIBLE` (`rufus.h`): an ISO, not disabled, not a `.wim`/`.esd` image, has `bootmgr` and a Windows installer, is not WinPE, and has at least one of `boot\etfsboot.com` (BIOS) or `efi\microsoft\boot\efisys.bin` (UEFI). The `has_etfsboot` and `has_efisys` fields are set during the scan (`check_iso_props`, `iso.c`). Ubuntu and other non-Windows ISOs are not eligible.

### 3.2 Pre-start checks (`rufus.c`)

- **`BootCheckThread`**: no drive required; skips the size and file system checks; shows the customization dialog **without** silent install (without changing the saved choice for USB mode); skips the GRUB/Syslinux downloads; makes sure oscdimg is available (`EnsureOscdimg`); cancels if Alt-Z (zero drive) is active.
- **`UM_FORMAT_START`**: asks for the destination file, refuses the source ISO itself (MSG_382), checks free space and starts `IsoOutputThread` instead of `FormatThread`.
- **`UM_FORMAT_COMPLETED`**: does not refresh the device list and shows the path of the created ISO.

### 3.3 Generation (`IsoOutputThread`, `isowrite.c`)

1. Creates `<temp>\RufusISO-XXXXXXXX\iso` and maps it to a free drive letter.
2. Temporarily sets globals (restored at the end): `fs_type = FS_NTFS` (prevents splitting an `install.wim` over 4 GB), `validate_md5sum = FALSE`, `enable_ntfs_compression = FALSE`, `preserve_timestamps = TRUE`.
3. `ExtractISO` to the mapped letter.
4. `ApplyWindowsCustomization`, if there is customization.
5. With CA 2023: extracts `Windows\Boot\DVD_EX\EFI\*\efisys_EX.bin` from `boot.wim` (preferring `en-US`) into `efi\microsoft\boot\`.
6. Extracts the additional-files ZIP, if any.
7. Writes the boot order file (`boot\`, `efi\`, `bootmgr`, `bootmgr.efi`, `sources\boot.wim`) outside the ISO tree.
8. Runs oscdimg:
   ```
   oscdimg -m -h -g -u2 -udfver102 -l"<label>" -yo<order>
           -bootdata:2#p0,e,b<X>:\boot\etfsboot.com#pEF,e,b<X>:\efi\microsoft\boot\<efisys.bin|efisys_EX.bin>
           <X>:\ "<destination>"
   ```
   Label: only letters, digits, `_`, `-` and `.`; anything else becomes `_`; 32 characters maximum.
9. Removes the drive letter and the workspace. On error or cancel it deletes the partial ISO, but only if oscdimg had already started writing it.

**Leftover cleanup:** on startup, `CleanupIsoOutputLeftovers` removes `RufusISO-` + 8 hex digit folders (with an `iso` subfolder) and drive letters pointing to them, left behind by a Rufus instance that was killed.

**Progress:** new `OP_CREATE_ISO` phase; `RunCommandWithProgressEx(op, ...)` lets oscdimg report in that phase (`RunCommandWithProgress` behaves as before).

### 3.4 Changed files

| File | Change |
|---|---|
| `src/isowrite.c`, `src/isowrite.h` | New: oscdimg, destination, free space, generation thread, cleanup |
| `src/rufus.c` | `iso_output`, checkbox and handler, `EnableControls`, `BootCheckThread`, `UM_FORMAT_START/COMPLETED` |
| `src/rufus.h` | `has_etfsboot`, `has_efisys`, `IS_ISO_OUTPUT_ELIGIBLE`, `OP_CREATE_ISO`, `RunCommandWithProgressEx` |
| `src/iso.c` | Detection of `etfsboot.com` and `efisys.bin` during the scan |
| `src/stdfn.c` | `RunCommandWithProgressEx` |
| `src/ui.c`, `src/ui_data.h` | Height and repositioning of the advanced section, progress phases |
| `src/rufus.rc`, `src/resource.h` | `IDC_ISO_OUTPUT` checkbox (1119) |
| `src/settings.h` | `SETTING_ISO_OUTPUT_DIR` |
| `res/loc/rufus.loc`, `src/localization_data.h` | `IDC_ISO_OUTPUT` and MSG_375 to MSG_382, in English and Portuguese |
| `.vs/rufus.vcxproj(.filters)`, `src/Makefile.am/.in` | Add `isowrite.c` |

**Build** (the machine has Visual Studio 2022; the project asks for v145):
```
MSBuild rufus.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

## 4. Key technical points

### 4.1 Why silent install is excluded

The silent install XML tries to rename partition 2 of disk 1 to `RUFUS_BOOT` and wipes disk 0 without showing any UI. The safeguard relies on that rename failing. An ISO has no Rufus USB drive: on a machine whose disk 1 is a data disk with a partition 2, the rename can succeed and disk 0 gets wiped. **The exclusion is mandatory, not a simplification.**

### 4.2 CA 2023 bootloaders in an ISO

- When booting an ISO in UEFI mode, the firmware does not use `efi\boot\bootx64.efi` from the tree but the FAT image referenced by the UEFI El Torito entry (`efisys.bin`). That image contains `cdboot.efi` signed by the **Microsoft Windows Production PCA 2011**, and Rufus does not modify it when writing to USB.
- The `boot.wim` of Windows 11 25H2 (26200) and 26H2 (26300) ISOs contains `Windows\Boot\DVD_EX\EFI\en-US\efisys_EX.bin`, whose `BOOTX64.EFI` is signed by the **Windows UEFI CA 2023**. Also confirmed in the pt-PT ISO (the folder is `en-US` in every ISO tested).
- In ISO mode, the UEFI entry points to `efisys_EX.bin` instead. The bootloader replacement in the tree done by `ApplyWindowsCustomization` still applies.
- 26H2 has no `bootmgr_EX.efi` in `EFI_EX`; Rufus 4.16 already handles that.
- BIOS boot is not affected (Secure Boot only applies to UEFI).

### 4.3 Other notes

- The "duplicate regional settings" option writes the time zone and languages **of the PC that created the ISO** into it; keep that in mind when sharing the ISO.
- Since 4.16, `boot.wim` is updated in place (`WIMLIB_WRITE_FLAG_UNSAFE_COMPACT`): less extra space needed, and the final ISO ends up slightly smaller than the original.
- Required space: source ISO size + ZIP + 5%, on both the workspace and the destination volume (the sum if they are the same volume).
- The oscdimg download was duplicated in `EnsureOscdimg` (~10 lines) instead of changing `SaveImage`, so the existing "save as ISO" is untouched.

## 5. Test results

**Proof of concept (oscdimg run by hand, Windows 11 26H2 pt-PT):** the generated ISO has the same size, tree and boot catalog as the official one. Both the normal and CA 2023 ISOs boot to the installer in VirtualBox with UEFI, Secure Boot and TPM 2.0; the CA 2023 ISO boots in BIOS mode in a VM created without UEFI (the BIOS boot path is identical in the normal ISO).

**Modified Rufus (26H2 en-US 26300, without CA 2023):**

| Check | Result |
|---|---|
| El Torito catalog | Two no-emulation entries: BIOS = `etfsboot.com`, UEFI = `efisys.bin` |
| Label | Same as the original |
| Workspace leftovers | None |
| Tree differences | Only those from the customization (`setup.exe` wrapper, empty `appraiserres.dll`, `$OEM$\$$\Panther\unattend.xml`) |
| `boot.wim` registry | `LabConfig` with TPM, Secure Boot and RAM bypass |
| VirtualBox UEFI + Secure Boot boot | Reaches the installer |
| VirtualBox BIOS boot | Reaches the installer |
| Boot via Ventoy on a physical UEFI PC | Reaches the installer (validates the content, not the El Torito entries) |

An independent code review found no regressions in USB writing; the fixes it proposed were applied before the commit.

## 6. To do

| Item | Notes |
|---|---|
| Direct boot on a physical PC with Secure Boot | Burned to DVD or mounted via remote management (no Ventoy) |
| CA 2023 ISO generated by the modified Rufus | Already worked in the proof of concept |
| Full install in a VM | Confirm local account, privacy, BitLocker, TPM/RAM bypass |
| Firmware trusting only one authority (2011 or 2023) | Not possible with default VirtualBox; Hyper-V Gen 2 allows choosing |
| USB writing regression | Same Windows and Ubuntu ISOs, with the new executable and the original 4.16; compare logs |
| Robustness | Cancel in each phase, low disk space, paths with spaces/accents, no network for oscdimg, Ubuntu not eligible |
| Other ISOs | Windows 10, ARM64 (UEFI entry only), Server, unofficial ISOs with a single-index `boot.wim` |

## 7. Updating to a new Rufus version

**Branches:** `master` mirrors `pbatard/rufus` (remote `upstream`, push disabled); our work lives on `develop`.

### 7.1 Sync

```
git fetch upstream
git checkout master
git merge --ff-only upstream/master
git push origin master
git checkout develop
git merge master
```

### 7.2 Where to expect conflicts

| Area | Why |
|---|---|
| `src/resource.h` | `IDC_ISO_OUTPUT` = 1119 and `MSG_375`…`MSG_382` may clash with new upstream IDs and messages. If they do, renumber to the next free values and update `localization_data.h` and `rufus.loc` |
| `res/loc/rufus.loc` | New upstream messages at the end of the en-US and pt-PT sections |
| `src/rufus.rc` | The checkbox sits after `IDC_UEFI_MEDIA_VALIDATION`, with the controls below shifted by +10 and the dialog height set to 336. Any upstream layout change means redoing these coordinates |
| `src/ui.c`, `src/ui_data.h` | Advanced section height (now ends at `IDC_ISO_OUTPUT`), `advanced_device_move_ids`/`toggle_ids` lists, low-resolution constants |
| `src/rufus.c` | `if (iso_output)` branches in `EnableControls`, `BootCheckThread`, `UM_FORMAT_START`, `UM_FORMAT_COMPLETED`, `PopulateProperties`, and the checkbox handler in `MainCallback` |
| `src/rufus.h` | Fields in `RUFUS_IMG_REPORT`, `OP_CREATE_ISO` (must stay before `OP_MAX`) |

### 7.3 What to check even without conflicts

ISO mode relies on upstream behaviour that can change without causing a merge conflict:

- **`ExtractISO(src, dst, scan)`** and **`ApplyWindowsCustomization(drive_letter, flags)`** still accept a drive letter. If they switch to a disk number or a drive handle, ISO mode breaks.
- **`check_iso_props`** only splits `install.wim` when `IS_FAT(fs_type)`. If that condition changes, the temporary `fs_type = FS_NTFS` may no longer be enough.
- **Customization options** (`UNATTEND_*` flags and the dialog in `BootCheckThread`): any new option that depends on the USB drive (like silent install) must also be excluded in ISO mode.
- **CA 2023:** the `Windows\Boot\DVD_EX\EFI\*\efisys_EX.bin` path inside `boot.wim`, and how `ApplyWindowsCustomization` replaces the bootloaders. If upstream starts handling `efisys.bin` itself, review `ExtractEfisysEx`.
- **oscdimg:** `OSCDIMG_URL` and `OSCDIMG_HASH` are shared with "save as ISO". `EnsureOscdimg` (`isowrite.c`) duplicates the download code from `SaveImage` (`vhd.c`); if upstream changes that block (for example, signature validation instead of a hash), apply the same change to `EnsureOscdimg`.
- **`RunCommandWithProgress`** (`stdfn.c`): now a macro over `RunCommandWithProgressEx`. Upstream changes to that function must be carried over to the `Ex` version.
- **Globals** used by the thread: `fs_type`, `validate_md5sum`, `preserve_timestamps`, `enable_ntfs_compression`, `unattend_xml_path`, `unattend_xml_flags`, `archive_path`, `temp_dir`. If any of them is renamed or changes meaning, update `IsoOutputThread`.

### 7.4 Minimum tests after each update

1. Build (command in section 3.4) with no errors or new warnings.
2. Create a recent Windows 11 ISO with customization, and another with CA 2023.
3. Check the El Torito catalog (two no-emulation entries) and the tree against the original.
4. Boot both in VirtualBox UEFI + Secure Boot and in BIOS, up to the installer.
5. Write the same ISO to a USB drive with ISO mode off and compare the log with the official Rufus of the same version.
6. Confirm that a non-Windows ISO (Ubuntu) leaves the checkbox disabled.

### 7.5 Risks

| Risk | Mitigation |
|---|---|
| Microsoft removes or changes oscdimg on the symbol server | The pinned hash detects the change; clear error message. Future alternative: IMAPI2 |
| ADK licence for downloading oscdimg at runtime | To be reviewed (internal use) |
| Antivirus blocks the embedded `setup.exe` or oscdimg | Same as USB mode |

## 8. References

- [Oscdimg Command-Line Options — Microsoft Learn](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/oscdimg-command-line-options?view=windows-11)
- [Updating Windows bootable media to use the PCA2023 signed boot manager — Microsoft Support](https://support.microsoft.com/en-us/servicing/os/windows/2025/02/updating-windows-bootable-media-to-use-the-pca2023-signed-boot-manager)
- [How to manage the Windows Boot Manager revocations (CVE-2023-24932, KB5025885) — Microsoft Support](https://support.microsoft.com/en-us/servicing/os/windows/2023/03/how-to-manage-the-windows-boot-manager-revocations-for-secure-boot-changes-associated-with-cve-2023)
