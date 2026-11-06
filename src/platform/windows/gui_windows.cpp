#include "cloudiemail/i18n.hpp"
#include "cloudiemail/server.hpp"
#include "cloudiemail/wizard.hpp"
#include <algorithm>
#include <commctrl.h>
#include <uxtheme.h>
#pragma comment(                                                                                             \
    linker,                                                                                                  \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <commdlg.h>
#include <shellapi.h>
#include <thread>
#include <windows.h>

namespace {
constexpr UINT tray_message = WM_APP + 1;
struct App {
    cm::Config config;
    std::shared_ptr<cm::Server> server;
    std::jthread worker;
    HWND window = nullptr, status = nullptr;
    NOTIFYICONDATAW tray{};
    std::mutex mutex;
    std::wstring error;
    std::filesystem::path path;
    std::unique_ptr<cm::Wizard> wizard;
    std::vector<HWND> setup_widgets, dashboard;
    std::map<std::string, HWND> inputs;
    HWND setup_log = nullptr, next_challenge = nullptr, cancel_setup = nullptr;
    std::string shown_setup_log;
    unsigned setup_revision = ~0u;
    HFONT font = nullptr, title_font = nullptr;
    HWND activity = nullptr;
    std::uint64_t log_sequence = 0;
    int scroll = 0, content_height = 0, form_bottom = 0;
    std::wstring shown_error;
    std::string language;
    std::map<HWND, RECT> positions;
    std::vector<std::pair<HWND, std::string>> translated;
    std::vector<HIMAGELIST> images;
    UINT dpi = 96;
    ~App() {
        for (auto image : images)
            ImageList_Destroy(image);
        if (font)
            DeleteObject(font);
        if (title_font)
            DeleteObject(title_font);
    }
};
std::wstring wide(const std::string &text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
std::string text(HWND control) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, 0);
    GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                    nullptr, nullptr);
    std::string result(static_cast<std::size_t>(bytes), 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes,
                        nullptr, nullptr);
    return result;
}
std::wstring edit_lines(const std::string &value) {
    std::string lines;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n' && (i == 0 || value[i - 1] != '\r'))
            lines += '\r';
        lines += value[i];
    }
    return wide(lines);
}
HWND setup_control(App *app, const wchar_t *kind, const std::string &label, DWORD style, int x, int y,
                   int width, int height, int id = 0) {
    auto control = CreateWindowExW(
        std::wstring(kind) == L"EDIT" && !(style & ES_READONLY) ? WS_EX_CLIENTEDGE : 0, kind,
        edit_lines(label).c_str(), WS_CHILD | WS_VISIBLE | (style & ~WS_BORDER), MulDiv(x, app->dpi, 96),
        MulDiv(y - app->scroll, app->dpi, 96), MulDiv(width, app->dpi, 96), MulDiv(height, app->dpi, 96),
        app->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
    SetWindowTheme(control, L"Explorer", nullptr);
    app->positions[control] = {x, y, width, height};
    app->setup_widgets.push_back(control);
    return control;
}
void layout(App *app) {
    if (!app->window)
        return;
    RECT client;
    GetClientRect(app->window, &client);
    const int width = MulDiv(client.right, 96, app->dpi), height = MulDiv(client.bottom, 96, app->dpi);
    bool ready = app->wizard->view().step == cm::SetupStep::Ready && !app->wizard->view().busy;
    if (ready) {
        ShowScrollBar(app->window, SB_VERT, FALSE);
        app->scroll = 0;
        auto move = [&](HWND c, int x, int y, int w, int h) {
            MoveWindow(c, MulDiv(x, app->dpi, 96), MulDiv(y, app->dpi, 96), MulDiv(w, app->dpi, 96),
                       MulDiv(h, app->dpi, 96), TRUE);
        };
        move(app->dashboard[0], 28, 22, width - 56, 42);
        move(app->status, 28, 74, width - 56, 50);
        for (int i = 0; i < 4; ++i)
            move(app->dashboard[static_cast<std::size_t>(i + 2)], 28 + i * (width - 56) / 4, 142,
                 (width - 80) / 4, 40);
        move(app->dashboard[6], 28, 206, width - 56, 26);
        move(app->activity, 28, 244, width - 56, std::max(100, height - 308));
        move(app->dashboard[8], 28, height - 48, 220, 32);
        return;
    }
    const int maxscroll = std::max(0, app->content_height - height);
    app->scroll = std::clamp(app->scroll, 0, maxscroll);
    SCROLLINFO info{sizeof(info),
                    SIF_RANGE | SIF_PAGE | SIF_POS,
                    0,
                    app->content_height,
                    static_cast<UINT>(height),
                    app->scroll,
                    0};
    SetScrollInfo(app->window, SB_VERT, &info, TRUE);
    for (const auto &[control, r] : app->positions) {
        int x = r.left, w = r.right;
        if (w == 890)
            w = width - 48;
        else if (x >= 474)
            x = 24 + (width - 48) / 2;
        if (w == 430)
            w = (width - 64) / 2;
        MoveWindow(control, MulDiv(x, app->dpi, 96), MulDiv(r.top - app->scroll, app->dpi, 96),
                   MulDiv(w, app->dpi, 96), MulDiv(r.bottom, app->dpi, 96), TRUE);
    }
    InvalidateRect(app->window, nullptr, TRUE);
}
void build_menu(App *app) {
    auto menu = CreateMenu(), file = CreatePopupMenu(), server = CreatePopupMenu(),
         settings = CreatePopupMenu(), language = CreatePopupMenu();
    auto add = [](HMENU target, UINT id, const char *key) {
        AppendMenuW(target, MF_STRING, id, wide(cm::tr(key)).c_str());
    };
    add(file, 4, "action.exit");
    add(server, 1, "action.start");
    add(server, 2, "action.stop");
    add(server, 3, "action.open_webmail");
    const char *keys[] = {"nav.overview",     "nav.network",  "nav.dns",
                          "nav.certificates", "nav.accounts", "nav.preferences"};
    for (UINT i = 0; i < 6; ++i)
        add(settings, 400 + i, keys[i]);
    const wchar_t *names[] = {L"Português (Brasil)", L"English", L"日本語", L"한국어", L"繁體中文"};
    const char *codes[] = {"pt-BR", "en", "ja", "ko", "zh-TW"};
    for (UINT i = 0; i < 5; ++i)
        AppendMenuW(language, MF_STRING | (cm::current_language() == codes[i] ? MF_CHECKED : 0), 500 + i,
                    names[i]);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), wide(cm::tr("nav.file")).c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(server), wide(cm::tr("nav.server")).c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), wide(cm::tr("nav.settings")).c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language), wide(cm::tr("nav.language")).c_str());
    auto old = GetMenu(app->window);
    SetMenu(app->window, menu);
    if (old)
        DestroyMenu(old);
    DrawMenuBar(app->window);
    for (auto &[control, key] : app->translated)
        SetWindowTextW(control, wide(cm::tr(key)).c_str());
    app->language = cm::current_language();
}
void refresh_setup(App *app) {
    if (!app->window)
        return;
    auto view = app->wizard->view();
    if (app->language != cm::current_language())
        build_menu(app);
    bool ready = view.step == cm::SetupStep::Ready && !view.busy;
    for (auto widget : app->dashboard)
        ShowWindow(widget, ready ? SW_SHOW : SW_HIDE);
    if (ready) {
        for (auto widget : app->setup_widgets)
            DestroyWindow(widget);
        app->setup_widgets.clear();
        app->inputs.clear();
        app->positions.clear();
        if (app->setup_revision != view.revision) {
            app->config = cm::Config::load(app->path);
            SetWindowTextW(app->window, wide("CloudieMail · " + app->config.domain).c_str());
            SetWindowTextW(app->dashboard.front(), wide("CloudieMail · " + app->config.domain).c_str());
            SetWindowTextW(app->status, wide(cm::tr("status.stopped")).c_str());
            app->setup_revision = view.revision;
            layout(app);
        }
        return;
    }
    if (view.revision != app->setup_revision) {
        for (auto widget : app->setup_widgets)
            DestroyWindow(widget);
        app->setup_widgets.clear();
        app->inputs.clear();
        app->positions.clear();
        app->scroll = 0;
        auto title = setup_control(app, L"STATIC", view.title, 0, 24, 20, 890, 36);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app->title_font), TRUE);
        setup_control(app, L"EDIT", view.body, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 24,
                      68, 890, 116);
        for (std::size_t i = 0; i < view.fields.size(); ++i) {
            const auto &field = view.fields[i];
            int x = 24 + static_cast<int>(i % 2) * 450, y = 204 + static_cast<int>(i / 2) * 74;
            if (field.checkbox) {
                auto control = setup_control(app, L"BUTTON", field.label,
                                             BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, x, y, 430, 50);
                SendMessageW(control, BM_SETCHECK, field.value == "1" ? BST_CHECKED : BST_UNCHECKED, 0);
                app->inputs[field.id] = control;
                continue;
            }
            setup_control(app, L"STATIC", field.label, 0, x, y, 430, 22);
            HWND input;
            if (!field.choices.empty()) {
                input = setup_control(app, L"COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x,
                                      y + 24, 430, 200);
                int selected = 0;
                for (std::size_t choice = 0; choice < field.choices.size(); ++choice) {
                    SendMessageW(input, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(wide(field.choices[choice].second).c_str()));
                    if (field.choices[choice].first == field.value)
                        selected = static_cast<int>(choice);
                }
                SendMessageW(input, CB_SETCURSEL, selected, 0);
            } else
                input =
                    setup_control(app, L"EDIT", field.value,
                                  WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (field.secret ? ES_PASSWORD : 0),
                                  x, y + 24, field.file ? 338 : 430, 26);
            app->inputs[field.id] = input;
            if (field.file)
                setup_control(app, L"BUTTON", cm::tr("action.browse"), BS_PUSHBUTTON | WS_TABSTOP, x + 344,
                              y + 24, 86, 26, 200 + static_cast<int>(i));
        }
        int y = 214 + static_cast<int>((view.fields.size() + 1) / 2) * 74;
        for (std::size_t i = 0; i < view.actions.size(); ++i)
            setup_control(app, L"BUTTON", view.actions[i].label, BS_PUSHBUTTON | WS_TABSTOP,
                          24 + static_cast<int>(i % 2) * 450, y + static_cast<int>(i / 2) * 36, 430, 34,
                          100 + static_cast<int>(i));
        y += static_cast<int>((view.actions.size() + 1) / 2) * 36 + 8;
        app->setup_log = setup_control(app, L"EDIT", "",
                                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
                                       24, y, 890, 150);
        app->shown_setup_log.clear();
        app->next_challenge = setup_control(app, L"BUTTON", cm::tr("action.txt_ready"),
                                            BS_PUSHBUTTON | WS_TABSTOP, 24, y + 158, 430, 30, 301);
        app->cancel_setup = setup_control(app, L"BUTTON", cm::tr("action.cancel"), BS_PUSHBUTTON | WS_TABSTOP,
                                          474, y + 158, 430, 30, 302);
        app->form_bottom = y;
        app->content_height = y + 24;
        app->setup_revision = view.revision;
        layout(app);
    }
    for (auto widget : app->setup_widgets) {
        auto id = GetDlgCtrlID(widget);
        if (widget != app->setup_log && id != 301 && id != 302)
            EnableWindow(widget, !view.busy);
    }
    bool show_log = view.busy || !view.log.empty();
    ShowWindow(app->setup_log, show_log ? SW_SHOW : SW_HIDE);
    int content_height = app->form_bottom + (show_log ? 200 : 24);
    if (content_height != app->content_height) {
        app->content_height = content_height;
        layout(app);
    }
    ShowWindow(app->next_challenge, view.busy && view.manual ? SW_SHOW : SW_HIDE);
    ShowWindow(app->cancel_setup, view.busy ? SW_SHOW : SW_HIDE);
    if (app->shown_setup_log != view.log) {
        SetWindowTextW(app->setup_log, edit_lines(view.log).c_str());
        app->shown_setup_log = view.log;
    }
}
void setup_command(App *app, int id) {
    if (id == 301) {
        app->wizard->next_challenge();
        return;
    }
    if (id == 302) {
        app->wizard->cancel();
        return;
    }
    auto view = app->wizard->view();
    if (view.busy)
        return;
    if (id >= 200 && id < 200 + static_cast<int>(view.fields.size())) {
        const auto &field = view.fields[static_cast<std::size_t>(id - 200)];
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = app->window;
        dialog.lpstrFile = path;
        dialog.nMaxFile = 32768;
        dialog.lpstrFilter = L"Certificados e chaves\0*.pem;*.crt;*.key\0Todos os arquivos\0*.*\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog))
            SetWindowTextW(app->inputs.at(field.id), path);
        return;
    }
    if (id < 100 || id >= 100 + static_cast<int>(view.actions.size()))
        return;
    std::map<std::string, std::string> values;
    for (const auto &field : view.fields) {
        auto input = app->inputs.at(field.id);
        if (!field.choices.empty())
            values[field.id] =
                field.choices.at(static_cast<std::size_t>(SendMessageW(input, CB_GETCURSEL, 0, 0))).first;
        else
            values[field.id] = field.checkbox
                                   ? (SendMessageW(input, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0")
                                   : text(input);
        if (field.secret)
            SetWindowTextW(input, L"");
    }
    app->wizard->perform(view.actions[static_cast<std::size_t>(id - 100)].id, std::move(values));
    refresh_setup(app);
}
void stop(App &app) {
    if (app.server)
        app.server->stop();
    if (app.worker.joinable())
        app.worker.join();
    app.server.reset();
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_WINDOW));
        SetTextColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_SIZE:
        if (app->window && app->dashboard.size() >= 9)
            layout(app);
        return 0;
    case WM_GETMINMAXINFO: {
        auto *limits = reinterpret_cast<MINMAXINFO *>(l);
        limits->ptMinTrackSize = {MulDiv(850, app->dpi, 96), MulDiv(600, app->dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        app->dpi = HIWORD(w);
        auto *r = reinterpret_cast<RECT *>(l);
        LOGFONTW font{};
        GetObjectW(app->font, sizeof(font), &font);
        font.lfHeight = -MulDiv(10, app->dpi, 72);
        DeleteObject(app->font);
        app->font = CreateFontIndirectW(&font);
        font.lfHeight = -MulDiv(19, app->dpi, 72);
        font.lfWeight = FW_SEMIBOLD;
        DeleteObject(app->title_font);
        app->title_font = CreateFontIndirectW(&font);
        EnumChildWindows(
            window,
            +[](HWND child, LPARAM value) -> BOOL {
                SendMessageW(child, WM_SETFONT,
                             reinterpret_cast<WPARAM>(reinterpret_cast<App *>(value)->font), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(app));
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        layout(app);
        return 0;
    }
    case WM_MOUSEWHEEL:
        app->scroll -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 60;
        layout(app);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(window, SB_VERT, &info);
        switch (LOWORD(w)) {
        case SB_LINEUP:
            app->scroll -= 30;
            break;
        case SB_LINEDOWN:
            app->scroll += 30;
            break;
        case SB_PAGEUP:
            app->scroll -= static_cast<int>(info.nPage);
            break;
        case SB_PAGEDOWN:
            app->scroll += static_cast<int>(info.nPage);
            break;
        case SB_THUMBTRACK:
            app->scroll = info.nTrackPos;
            break;
        }
        layout(app);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) >= 400 && LOWORD(w) < 505) {
            if (app->server || app->wizard->view().busy) {
                MessageBoxW(window, wide(cm::tr("status.stop_first")).c_str(), L"CloudieMail", MB_OK);
                return 0;
            }
            try {
                if (LOWORD(w) >= 500) {
                    const char *codes[] = {"pt-BR", "en", "ja", "ko", "zh-TW"};
                    app->wizard->language(codes[LOWORD(w) - 500]);
                } else if (LOWORD(w) < 406) {
                    const char *actions[] = {"done", "back", "dns", "certificate", "accounts", "preferences"};
                    app->wizard->perform(actions[LOWORD(w) - 400], {});
                }
                refresh_setup(app);
            } catch (const std::exception &e) {
                MessageBoxW(window, wide(e.what()).c_str(), L"CloudieMail", MB_ICONERROR);
            }
            return 0;
        }
        if (LOWORD(w) >= 100) {
            setup_command(app, LOWORD(w));
            return 0;
        }
        if (LOWORD(w) == 1) {
            try {
                if (app->wizard->view().step != cm::SetupStep::Ready)
                    throw std::runtime_error(cm::tr("status.setup_first"));
                if (app->server && app->server->running())
                    break;
                stop(*app);
                app->server = std::make_shared<cm::Server>(app->config);
                app->log_sequence = 0;
                {
                    std::lock_guard lock(app->mutex);
                    app->error.clear();
                }
                auto server = app->server;
                app->worker = std::jthread([app, server] {
                    try {
                        server->run();
                    } catch (const std::exception &e) {
                        std::lock_guard lock(app->mutex);
                        app->error = wide(e.what());
                    }
                });
            } catch (const std::exception &e) {
                MessageBoxW(window, wide(e.what()).c_str(), L"CloudieMail", MB_ICONERROR);
            }
        } else if (LOWORD(w) == 2) {
            stop(*app);
            SetWindowTextW(app->status, wide(cm::tr("status.stopped")).c_str());
        } else if (LOWORD(w) == 3) {
            auto url =
                wide("https://" + app->config.webmail_host() +
                     (app->config.https_port == 443 ? "" : ":" + std::to_string(app->config.https_port)));
            ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (LOWORD(w) == 5) {
            if (app->server)
                MessageBoxW(window, wide(cm::tr("status.stop_first")).c_str(), L"CloudieMail", MB_OK);
            else {
                app->wizard->perform("preferences", {});
                refresh_setup(app);
            }
        } else if (LOWORD(w) == 6) {
            SetWindowTextW(app->activity, L"");
        } else if (LOWORD(w) == 4)
            DestroyWindow(window);
        return 0;
    case WM_TIMER: {
        refresh_setup(app);
        auto append = [&](const std::wstring &line) {
            if (GetWindowTextLengthW(app->activity) > 60000)
                SetWindowTextW(app->activity, L"");
            SendMessageW(app->activity, EM_SETSEL, static_cast<WPARAM>(-1), -1);
            SendMessageW(app->activity, EM_REPLACESEL, FALSE,
                         reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
        };
        if (app->server)
            for (const auto &event : app->server->logs(app->log_sequence)) {
                app->log_sequence = event.at("id").get<std::uint64_t>();
                append(wide(event.at("time").get<std::string>() + "  " +
                            cm::tr(event.at("event").get<std::string>())));
            }
        std::lock_guard lock(app->mutex);
        if (app->error != app->shown_error) {
            if (!app->error.empty())
                append(app->error);
            app->shown_error = app->error;
        }
        if (!app->error.empty())
            SetWindowTextW(app->status, app->error.c_str());
        else if (app->server && app->server->running())
            SetWindowTextW(app->status, wide(cm::tr("server.started")).c_str());
        return 0;
    }
    case tray_message:
        if (l == WM_LBUTTONUP || l == WM_RBUTTONUP) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
        }
        return 0;
    case WM_CLOSE:
        if (app->tray.hWnd) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        stop(*app);
        Shell_NotifyIconW(NIM_DELETE, &app->tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::filesystem::path config = argc > 1 ? argv[1] : L"config/local.json";
        LocalFree(argv);
        App app;
        app.dpi = GetDpiForSystem();
        app.font = CreateFontW(-MulDiv(10, app.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        app.title_font = CreateFontW(-MulDiv(19, app.dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        app.path = config;
        app.wizard = std::make_unique<cm::Wizard>(config);
        if (std::filesystem::exists(config))
            app.config = cm::Config::load(config);
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = instance;
        cls.lpszClassName = L"CloudieMailWindow";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&cls);
        auto window = CreateWindowW(cls.lpszClassName, L"CloudieMail",
                                    WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                    CW_USEDEFAULT, MulDiv(1040, app.dpi, 96), MulDiv(820, app.dpi, 96),
                                    nullptr, nullptr, instance, &app);
        if (!window)
            return 1;
        app.window = window;
        auto control = [&](const wchar_t *kind, const std::string &label, DWORD style, int id,
                           const char *key = "") {
            HWND c = CreateWindowExW(std::wstring(kind) == L"EDIT" ? WS_EX_CLIENTEDGE : 0, kind,
                                     wide(label).c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
            SetWindowTheme(c, L"Explorer", nullptr);
            app.dashboard.push_back(c);
            if (*key)
                app.translated.emplace_back(c, key);
            return c;
        };
        auto title = control(L"STATIC", "CloudieMail · " + app.config.domain, 0, 0);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app.title_font), TRUE);
        app.status = control(L"STATIC", cm::tr("status.stopped"), 0, 0);
        const char *keys[] = {"action.start", "action.stop", "action.open_webmail", "nav.settings"};
        const int ids[] = {1, 2, 3, 5};
        const SHSTOCKICONID glyphs[] = {SIID_NETWORKCONNECT, SIID_ERROR, SIID_INTERNET, SIID_SETTINGS};
        for (int i = 0; i < 4; ++i) {
            auto button = control(L"BUTTON", cm::tr(keys[i]), BS_PUSHBUTTON | WS_TABSTOP, ids[i], keys[i]);
            SHSTOCKICONINFO icon{sizeof(icon)};
            if (SUCCEEDED(SHGetStockIconInfo(glyphs[i], SHGSI_ICON | SHGSI_SMALLICON, &icon))) {
                int size = MulDiv(16, app.dpi, 96);
                auto images = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 1, 1);
                ImageList_AddIcon(images, icon.hIcon);
                DestroyIcon(icon.hIcon);
                app.images.push_back(images);
                BUTTON_IMAGELIST image{images, {10, 0, 8, 0}, BUTTON_IMAGELIST_ALIGN_LEFT};
                SendMessageW(button, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&image));
            }
        }
        control(L"STATIC", cm::tr("nav.activity"), 0, 0, "nav.activity");
        app.activity =
            control(L"EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 0);
        control(L"BUTTON", cm::tr("action.clear_log"), BS_PUSHBUTTON | WS_TABSTOP, 6, "action.clear_log");
        build_menu(&app);
        layout(&app);
        app.tray.cbSize = sizeof(app.tray);
        app.tray.hWnd = window;
        app.tray.uID = 1;
        app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        app.tray.uCallbackMessage = tray_message;
        app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(app.tray.szTip, L"CloudieMail");
        if (!Shell_NotifyIconW(NIM_ADD, &app.tray))
            app.tray.hWnd = nullptr;
        refresh_setup(&app);
        SetTimer(window, 1, 200, nullptr);
        ShowWindow(window, show);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return 0;
    } catch (const std::exception &e) {
        MessageBoxW(nullptr, wide(e.what()).c_str(), L"CloudieMail", MB_ICONERROR);
        return 1;
    }
}
