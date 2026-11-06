#include "cloudiemail/setup.hpp"
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace cm {
namespace {
bool filtered(std::string name) {
    for (auto &c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name.starts_with("LEGO_") || name.starts_with("CF_") || name.starts_with("CLOUDFLARE_") ||
           name.starts_with("MANUAL_") || name == "LD_PRELOAD" || name == "LD_LIBRARY_PATH";
}
#ifdef _WIN32
std::wstring wide(const std::string &text) {
    auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                 nullptr, 0);
    if (!n && !text.empty())
        throw std::runtime_error("Texto UTF-8 invalido");
    std::wstring result(static_cast<std::size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), n);
    return result;
}
std::string narrow(const std::wstring &text) {
    auto n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                 nullptr);
    std::string result(static_cast<std::size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), n, nullptr,
                        nullptr);
    return result;
}
std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    void close() {
        if (value)
            CloseHandle(value);
        value = nullptr;
    }
};
#else
struct Fd {
    int value = -1;
    ~Fd() {
        if (value >= 0)
            close(value);
    }
    void reset() {
        if (value >= 0)
            close(value);
        value = -1;
    }
};
#endif
} // namespace
#ifndef _WIN32
std::filesystem::path linux_executable_directory() {
    const auto *path = reinterpret_cast<const char *>(getauxval(AT_EXECFN));
    if (!path)
        throw std::runtime_error("Caminho do executavel indisponivel");
    return std::filesystem::absolute(path).parent_path();
}
#endif
int run_child(const std::filesystem::path &binary, const std::vector<std::string> &arguments,
              const std::map<std::string, std::string> &environment, const Progress &output,
              std::atomic_bool &cancel, std::atomic_uint &continue_input) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
    auto emit = [&](std::string text) {
        for (const auto &[name, secret] : environment) {
            if (secret.empty() || name.find("TOKEN") == name.npos)
                continue;
            for (auto pos = text.find(secret); pos != text.npos; pos = text.find(secret))
                text.replace(pos, secret.size(), "[token oculto]");
        }
        output(text);
    };
#ifdef _WIN32
    SECURITY_ATTRIBUTES attrs{sizeof(attrs), nullptr, TRUE};
    Handle input_read, input_write, output_read, output_write;
    if (!CreatePipe(&input_read.value, &input_write.value, &attrs, 0) ||
        !CreatePipe(&output_read.value, &output_write.value, &attrs, 0))
        throw std::runtime_error("Nao foi possivel criar canal ACME");
    SetHandleInformation(input_write.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0);
    std::map<std::wstring, std::wstring> variables;
    auto *block = GetEnvironmentStringsW();
    if (!block)
        throw std::runtime_error("Ambiente indisponivel");
    for (auto *p = block; *p; p += wcslen(p) + 1) {
        std::wstring item(p);
        auto separator = item.find(L'=');
        if (separator == 0 || separator == item.npos)
            continue;
        auto name = item.substr(0, separator);
        if (!filtered(narrow(name)))
            variables[name] = item.substr(separator + 1);
    }
    FreeEnvironmentStringsW(block);
    for (const auto &[name, value] : environment)
        variables[wide(name)] = wide(value);
    std::wstring child_environment;
    for (const auto &[name, value] : variables)
        child_environment += name + L"=" + value + L'\0';
    child_environment += L'\0';
    std::wstring command = quote(binary.wstring());
    for (const auto &argument : arguments)
        command += L" " + quote(wide(argument));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input_read.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output_write.value;
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attribute_data(attribute_bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_data.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes))
        throw std::runtime_error("Nao foi possivel isolar o processo ACME");
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Attributes() {
            DeleteProcThreadAttributeList(value);
        }
    } attributes{startup.lpAttributeList};
    HANDLE inherited[] = {input_read.value, output_write.value};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr))
        throw std::runtime_error("Nao foi possivel restringir os handles ACME");
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        child_environment.data(), nullptr, &startup.StartupInfo, &process))
        throw std::runtime_error("Nao foi possivel executar cliente ACME");
    Handle child{process.hProcess}, thread{process.hThread};
    input_read.close();
    output_write.close();
    try {
        for (;;) {
            char buffer[4096];
            DWORD available = 0, count = 0;
            while (PeekNamedPipe(output_read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
                if (!ReadFile(output_read.value, buffer, sizeof(buffer), &count, nullptr) || !count)
                    break;
                emit(std::string(buffer, count));
            }
            if (WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0)
                break;
            if (cancel || std::chrono::steady_clock::now() > deadline) {
                TerminateProcess(child.value, 1);
                WaitForSingleObject(child.value, INFINITE);
                return 1;
            }
            if (continue_input.exchange(0))
                WriteFile(input_write.value, "\n", 1, &count, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (...) {
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, INFINITE);
        throw;
    }
    DWORD status = 1;
    char trailing[4096];
    DWORD available = 0, count = 0;
    while (PeekNamedPipe(output_read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
        if (!ReadFile(output_read.value, trailing, sizeof(trailing), &count, nullptr) || !count)
            break;
        emit(std::string(trailing, count));
    }
    GetExitCodeProcess(child.value, &status);
    return static_cast<int>(status);
#else
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) != 0)
        throw std::runtime_error("Falha ao criar canal ACME");
    Fd input_read{in_pipe[0]}, input_write{in_pipe[1]};
    if (pipe2(out_pipe, O_CLOEXEC) != 0)
        throw std::runtime_error("Falha ao criar canal ACME");
    Fd output_read{out_pipe[0]}, output_write{out_pipe[1]};
    std::map<std::string, std::string> variables;
    for (auto **p = environ; *p; ++p) {
        std::string item(*p);
        auto separator = item.find('=');
        if (separator != item.npos && !filtered(item.substr(0, separator)))
            variables[item.substr(0, separator)] = item.substr(separator + 1);
    }
    for (const auto &[name, value] : environment)
        variables[name] = value;
    std::vector<std::string> env_strings, argv_strings{binary.string()};
    for (const auto &[name, value] : variables)
        env_strings.push_back(name + "=" + value);
    argv_strings.insert(argv_strings.end(), arguments.begin(), arguments.end());
    std::vector<char *> envp, argv;
    for (auto &item : env_strings)
        envp.push_back(item.data());
    for (auto &item : argv_strings)
        argv.push_back(item.data());
    envp.push_back(nullptr);
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input_read.value, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_write.value, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_write.value, STDERR_FILENO);
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    pid_t child = 0;
    int error = posix_spawn(&child, binary.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (error)
        throw std::runtime_error("Nao foi possivel executar cliente ACME");
    input_read.reset();
    output_write.reset();
    fcntl(output_read.value, F_SETFL, O_NONBLOCK);
    int status = 0;
    try {
        for (;;) {
            char buffer[4096];
            ssize_t count;
            while ((count = read(output_read.value, buffer, sizeof(buffer))) > 0)
                emit(std::string(buffer, static_cast<std::size_t>(count)));
            if (waitpid(child, &status, WNOHANG) == child)
                break;
            if (cancel || std::chrono::steady_clock::now() > deadline) {
                kill(child, SIGKILL);
                waitpid(child, &status, 0);
                return 1;
            }
            if (continue_input.exchange(0)) {
                auto written = write(input_write.value, "\n", 1);
                (void)written;
            }
            pollfd descriptor{output_read.value, POLLIN, 0};
            poll(&descriptor, 1, 50);
        }
        char buffer[4096];
        ssize_t count;
        while ((count = read(output_read.value, buffer, sizeof(buffer))) > 0)
            emit(std::string(buffer, static_cast<std::size_t>(count)));
    } catch (...) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        throw;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
} // namespace cm
