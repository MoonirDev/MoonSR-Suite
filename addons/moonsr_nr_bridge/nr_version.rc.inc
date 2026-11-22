// Shared version resource for the neural rendering bridge binaries. The version comes from the root CMakeLists.txt
// through the generated core/version.h. Define NR_FILE_DESCRIPTION before including this file.
#include "core/version.h"

VS_VERSION_INFO VERSIONINFO
FILEVERSION MOONSR_VERSION_RC
PRODUCTVERSION MOONSR_VERSION_RC
FILEOS VOS_NT_WINDOWS32
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "FileDescription", NR_FILE_DESCRIPTION
            VALUE "ProductName", "MoonSR"
            VALUE "CompanyName", "MoonDev"
            VALUE "LegalCopyright", "GPL-3.0-only. Part of MoonSR."
            VALUE "FileVersion", MOONSR_VERSION_STRING
            VALUE "ProductVersion", MOONSR_VERSION_STRING
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
