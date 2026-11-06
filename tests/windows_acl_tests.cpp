#include "cloudiemail/setup.hpp"
#include <aclapi.h>
#include <fstream>
#include <iostream>
#include <windows.h>

namespace {
void verify(const std::filesystem::path &path, bool directory, PSID user) {
    auto name = path.wstring();
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto error = GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                       nullptr, &acl, nullptr, &descriptor);
    if (error != ERROR_SUCCESS)
        throw std::runtime_error("Cannot read ACL: " + std::to_string(error));
    struct Free {
        PSECURITY_DESCRIPTOR p;
        ~Free() {
            LocalFree(p);
        }
    } cleanup{descriptor};
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision) || !acl)
        throw std::runtime_error("Cannot inspect the security descriptor: " + cm::path_utf8(path));
    if (acl->AceCount != 2)
        throw std::runtime_error("Expected user and SYSTEM only (2 ACEs), got " +
                                 std::to_string(acl->AceCount) + ": " + cm::path_utf8(path));
    if (directory && !(control & SE_DACL_PROTECTED))
        throw std::runtime_error("Directory still inherits permissions");
    BYTE system[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(system);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size))
        throw std::runtime_error("SYSTEM SID unavailable");
    bool found_user = false, found_system = false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void *entry = nullptr;
        if (!GetAce(acl, i, &entry))
            throw std::runtime_error("Cannot read ACE");
        auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            (ace->Mask & FILE_ALL_ACCESS) != FILE_ALL_ACCESS)
            throw std::runtime_error("Unexpected ACE access mask");
        if (EqualSid(&ace->SidStart, user))
            found_user = true;
        else if (EqualSid(&ace->SidStart, system))
            found_system = true;
        else
            throw std::runtime_error("Unexpected principal has access");
        if (directory && (ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) !=
                             (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))
            throw std::runtime_error("Child inheritance missing");
    }
    if (!found_user || !found_system)
        throw std::runtime_error("Required principal missing");
}
} // namespace
int main() {
    try {
        cm::crypto_init();
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            throw std::runtime_error("Token unavailable");
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> info(bytes);
        auto ok = GetTokenInformation(token, TokenUser, info.data(), bytes, &bytes);
        CloseHandle(token);
        if (!ok)
            throw std::runtime_error("User SID unavailable");
        auto user = reinterpret_cast<TOKEN_USER *>(info.data())->User.Sid;
        auto directory = std::filesystem::temp_directory_path() / ("CloudieMail ACL " + cm::random_id());
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove_all(p, error);
            }
        } cleanup{directory};
        cm::private_directory(directory);
        verify(directory, true, user);
        cm::save_json(directory / "config.json", {{"test", true}});
        verify(directory / "config.json", false, user);
        cm::private_directory(directory); // Existing installation / resume must also work.
        verify(directory, true, user);
        verify(directory / "config.json", false, user);
        std::cout << "4 Windows ACL checks passed (directory, child inheritance, repeated protection)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
