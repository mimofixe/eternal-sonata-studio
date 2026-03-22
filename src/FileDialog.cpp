#include "FileDialog.h"
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

std::string FileDialog::OpenFile(const char* filter) {
    OPENFILENAMEA ofn;
    CHAR szFile[MAX_PATH] = { 0 };

    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::string(ofn.lpstrFile);
    }

    return std::string();
}

std::string FileDialog::SaveFile(const char* filter) {
    OPENFILENAMEA ofn;
    CHAR szFile[MAX_PATH] = { 0 };

    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameA(&ofn) == TRUE) {
        return std::string(ofn.lpstrFile);
    }

    return std::string();
}

std::string FileDialog::OpenFolder() {
    BROWSEINFOA bi;
    CHAR szDir[MAX_PATH] = { 0 };

    ZeroMemory(&bi, sizeof(BROWSEINFO));
    bi.lpszTitle = "Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);

    if (pidl != nullptr) {
        SHGetPathFromIDListA(pidl, szDir);
        CoTaskMemFree(pidl);
        return std::string(szDir);
    }

    return std::string();
}