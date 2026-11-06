#include "cloudiemail/dns.hpp"
#include "cloudiemail/i18n.hpp"
#include "cloudiemail/server.hpp"
#include "cloudiemail/wizard.hpp"
#include <csignal>
#include <fstream>
#include <gtk/gtk.h>
#include <sys/stat.h>
#include <thread>

namespace {
volatile std::sig_atomic_t gui_interrupted = 0;
void interrupt_gui(int) {
    gui_interrupted = 1;
}
struct App {
    GtkApplication *application = nullptr;
    GtkWindow *window = nullptr;
    GtkLabel *status = nullptr;
    GtkEntry *email = nullptr, *password = nullptr, *ip = nullptr;
    GtkTextBuffer *dns = nullptr;
    std::filesystem::path path;
    cm::Config config;
    std::shared_ptr<cm::Server> server;
    std::jthread worker;
    std::mutex mutex;
    std::string failure;
    GDBusConnection *bus = nullptr;
    guint registration = 0, watch = 0;
    bool tray = false;
    std::unique_ptr<cm::Wizard> wizard;
    GtkWidget *stack = nullptr, *setup_box = nullptr, *setup_fields = nullptr, *setup_actions = nullptr;
    GtkWidget *start_button = nullptr, *setup_cancel = nullptr, *setup_continue = nullptr;
    GtkWidget *setup_log_scroll = nullptr;
    GtkTextBuffer *setup_log = nullptr;
    GtkLabel *subtitle = nullptr;
    std::map<std::string, GtkWidget *> setup_inputs;
    unsigned setup_revision = ~0u;
    std::uint64_t log_sequence = 0;
    GtkTextBuffer *activity = nullptr;
    GtkWidget *navigation = nullptr;
    std::string shown_failure, shown_language;
    std::vector<std::pair<GtkWidget *, std::string>> translations;
};
void refresh_setup(App *app);
void set_status(App *app, const std::string &text) {
    gtk_label_set_text(app->status, text.c_str());
}
void stop(App *app) {
    if (app->server)
        app->server->stop();
    if (app->worker.joinable())
        app->worker.join();
    app->server.reset();
}
void start(GtkButton *, gpointer data) {
    auto *app = static_cast<App *>(data);
    if (app->server && app->server->running())
        return;
    stop(app);
    try {
        if (app->wizard->view().step != cm::SetupStep::Ready)
            throw std::runtime_error(cm::tr("status.setup_first"));
        app->config = cm::Config::load(app->path);
        app->server = std::make_shared<cm::Server>(app->config);
        app->log_sequence = 0;
        {
            std::lock_guard lock(app->mutex);
            app->failure.clear();
        }
        auto server = app->server;
        app->worker = std::jthread([app, server] {
            try {
                server->run();
            } catch (const std::exception &e) {
                std::lock_guard lock(app->mutex);
                app->failure = e.what();
            }
        });
        set_status(app, cm::tr("status.starting"));
    } catch (const std::exception &e) {
        set_status(app, e.what());
    }
}
void stop_clicked(GtkButton *, gpointer data) {
    auto *app = static_cast<App *>(data);
    stop(app);
    set_status(app, cm::tr("status.stopped"));
}
gboolean update(gpointer data) {
    auto *app = static_cast<App *>(data);
    if (gui_interrupted) {
        g_application_quit(G_APPLICATION(app->application));
        return G_SOURCE_REMOVE;
    }
    refresh_setup(app);
    auto append = [&](const std::string &line) {
        if (!app->activity)
            return;
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->activity, &end);
        gtk_text_buffer_insert(app->activity, &end, (line + "\n").c_str(), -1);
        if (gtk_text_buffer_get_line_count(app->activity) > 500) {
            GtkTextIter first, cut;
            gtk_text_buffer_get_start_iter(app->activity, &first);
            gtk_text_buffer_get_iter_at_line(app->activity, &cut, 100);
            gtk_text_buffer_delete(app->activity, &first, &cut);
        }
    };
    if (app->server)
        for (const auto &event : app->server->logs(app->log_sequence)) {
            app->log_sequence = event.at("id").get<std::uint64_t>();
            append(event.at("time").get<std::string>() + "  " + cm::tr(event.at("event").get<std::string>()));
        }
    std::lock_guard lock(app->mutex);
    if (app->failure != app->shown_failure) {
        if (!app->failure.empty())
            append(app->failure);
        app->shown_failure = app->failure;
    }
    if (!app->failure.empty())
        set_status(app, app->failure);
    else if (app->server && app->server->running())
        set_status(app, cm::tr("server.started"));
    return G_SOURCE_CONTINUE;
}
void open_webmail(GtkButton *, gpointer data) {
    auto *app = static_cast<App *>(data);
    auto uri = "https://" + app->config.webmail_host() +
               (app->config.https_port == 443 ? "" : ":" + std::to_string(app->config.https_port));
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
}
void tray_method(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                 GVariant *, GDBusMethodInvocation *invocation, gpointer data) {
    auto *app = static_cast<App *>(data);
    if (std::string(method) == "Activate" || std::string(method) == "ContextMenu" ||
        std::string(method) == "SecondaryActivate")
        gtk_window_present(app->window);
    g_dbus_method_invocation_return_value(invocation, nullptr);
}
GVariant *tray_property(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *property,
                        GError **, gpointer) {
    std::string p = property;
    if (p == "Category")
        return g_variant_new_string("SystemServices");
    if (p == "Id")
        return g_variant_new_string("cloudiemail");
    if (p == "Title")
        return g_variant_new_string("CloudieMail");
    if (p == "Status")
        return g_variant_new_string("Active");
    if (p == "IconName")
        return g_variant_new_string("mail-unread");
    if (p == "ItemIsMenu")
        return g_variant_new_boolean(false);
    return nullptr;
}
void watcher_appeared(GDBusConnection *bus, const gchar *, const gchar *, gpointer data) {
    auto *app = static_cast<App *>(data);
    GError *error = nullptr;
    auto *value = g_dbus_connection_call_sync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", "org.kde.StatusNotifierWatcher", "IsStatusNotifierHostRegistered"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
    if (value) {
        GVariant *inner = nullptr;
        g_variant_get(value, "(v)", &inner);
        app->tray = g_variant_get_boolean(inner);
        g_variant_unref(inner);
        g_variant_unref(value);
    }
    if (error)
        g_error_free(error);
    if (app->tray)
        g_dbus_connection_call(bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                               "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                               g_variant_new("(s)", "/StatusNotifierItem"), nullptr, G_DBUS_CALL_FLAGS_NONE,
                               1000, nullptr, nullptr, nullptr);
}
void watcher_vanished(GDBusConnection *, const gchar *, gpointer data) {
    static_cast<App *>(data)->tray = false;
}
void setup_tray(App *app) {
    const char *xml =
        "<node><interface name='org.kde.StatusNotifierItem'><method name='Activate'><arg type='i' "
        "direction='in'/><arg type='i' direction='in'/></method><method name='ContextMenu'><arg type='i' "
        "direction='in'/><arg type='i' direction='in'/></method><method name='SecondaryActivate'><arg "
        "type='i' direction='in'/><arg type='i' direction='in'/></method><property name='Category' type='s' "
        "access='read'/><property name='Id' type='s' access='read'/><property name='Title' type='s' "
        "access='read'/><property name='Status' type='s' access='read'/><property name='IconName' type='s' "
        "access='read'/><property name='ItemIsMenu' type='b' access='read'/></interface></node>";
    app->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!app->bus)
        return;
    auto *node = g_dbus_node_info_new_for_xml(xml, nullptr);
    static const GDBusInterfaceVTable table = {tray_method, tray_property, nullptr, {nullptr}};
    app->registration = g_dbus_connection_register_object(app->bus, "/StatusNotifierItem",
                                                          node->interfaces[0], &table, app, nullptr, nullptr);
    g_dbus_node_info_unref(node);
    app->watch = g_bus_watch_name_on_connection(app->bus, "org.kde.StatusNotifierWatcher",
                                                G_BUS_NAME_WATCHER_FLAGS_NONE, watcher_appeared,
                                                watcher_vanished, app, nullptr);
}
gboolean close_window(GtkWindow *window, gpointer data) {
    auto *app = static_cast<App *>(data);
    if (app->tray) {
        gtk_widget_set_visible(GTK_WIDGET(window), false);
        return true;
    }
    g_application_quit(G_APPLICATION(app->application));
    return false;
}
GtkWidget *button(const char *text, GCallback callback, App *app) {
    auto *widget = gtk_button_new_with_label(text);
    g_signal_connect(widget, "clicked", callback, app);
    return widget;
}
void choose_setup_file(GtkButton *button, gpointer) {
    auto *entry = GTK_ENTRY(g_object_get_data(G_OBJECT(button), "entry"));
    auto *chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, cm::tr("action.browse").c_str());
    g_object_ref(entry);
    gtk_file_dialog_open(
        chooser, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))), nullptr,
        +[](GObject *source, GAsyncResult *result, gpointer value) {
            auto *target = GTK_ENTRY(value);
            GError *error = nullptr;
            auto *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
            if (file) {
                auto *path = g_file_get_path(file);
                if (path)
                    gtk_editable_set_text(GTK_EDITABLE(target), path);
                g_free(path);
                g_object_unref(file);
            }
            if (error)
                g_error_free(error);
            g_object_unref(target);
        },
        entry);
    g_object_unref(chooser);
}
void setup_action(GtkButton *button, gpointer value) {
    auto *app = static_cast<App *>(value);
    auto action = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "action"));
    auto view = app->wizard->view();
    std::map<std::string, std::string> values;
    for (const auto &field : view.fields) {
        auto *input = app->setup_inputs.at(field.id);
        if (!field.choices.empty()) {
            auto index = gtk_drop_down_get_selected(GTK_DROP_DOWN(input));
            values[field.id] = field.choices.at(index).first;
        } else
            values[field.id] = field.checkbox
                                   ? (gtk_check_button_get_active(GTK_CHECK_BUTTON(input)) ? "1" : "0")
                                   : gtk_editable_get_text(GTK_EDITABLE(input));
        if (field.secret)
            gtk_editable_set_text(GTK_EDITABLE(input), "");
    }
    app->wizard->perform(action, std::move(values));
}
void refresh_setup(App *app) {
    if (!app->stack)
        return;
    auto view = app->wizard->view();
    if (app->shown_language != cm::current_language()) {
        app->shown_language = cm::current_language();
        for (auto &[widget, key] : app->translations)
            gtk_label_set_text(GTK_LABEL(widget), cm::tr(key).c_str());
    }
    if (app->navigation)
        gtk_widget_set_sensitive(app->navigation, !view.busy);
    if (view.step == cm::SetupStep::Ready && !view.busy) {
        if (app->setup_revision != view.revision) {
            // The embedded engine may install signal handlers during first database use.
            std::signal(SIGTERM, interrupt_gui);
            std::signal(SIGINT, interrupt_gui);
            app->config = cm::Config::load(app->path);
            gtk_label_set_text(app->subtitle,
                               (app->config.domain + " · " + app->config.webmail_host()).c_str());
            set_status(app, cm::tr("status.stopped"));
            app->setup_revision = view.revision;
        }
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "dashboard");
        gtk_widget_set_sensitive(app->start_button, true);
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "setup");
    gtk_widget_set_sensitive(app->start_button, false);
    if (app->setup_revision != view.revision) {
        gtk_window_set_focus(app->window, nullptr);
        while (auto *child = gtk_widget_get_first_child(app->setup_box))
            gtk_box_remove(GTK_BOX(app->setup_box), child);
        app->setup_inputs.clear();
        auto *title = gtk_label_new(view.title.c_str());
        gtk_widget_add_css_class(title, "title-1");
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_box_append(GTK_BOX(app->setup_box), title);
        auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        auto *content_scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(content_scroll), content);
        gtk_widget_set_vexpand(content_scroll, true);
        gtk_box_append(GTK_BOX(app->setup_box), content_scroll);
        auto *body = gtk_label_new(view.body.c_str());
        gtk_label_set_wrap(GTK_LABEL(body), true);
        gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(body), 80);
        gtk_label_set_xalign(GTK_LABEL(body), 0);
        gtk_label_set_selectable(GTK_LABEL(body), true);
        gtk_box_append(GTK_BOX(content), body);
        app->setup_fields = gtk_grid_new();
        gtk_grid_set_column_homogeneous(GTK_GRID(app->setup_fields), true);
        gtk_grid_set_column_spacing(GTK_GRID(app->setup_fields), 24);
        gtk_grid_set_row_spacing(GTK_GRID(app->setup_fields), 14);
        gtk_box_append(GTK_BOX(content), app->setup_fields);
        int field_index = 0;
        for (const auto &field : view.fields) {
            auto *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_set_hexpand(cell, true);
            gtk_grid_attach(GTK_GRID(app->setup_fields), cell, field_index % 2, field_index / 2, 1, 1);
            ++field_index;
            if (field.checkbox) {
                auto *input = gtk_check_button_new();
                auto *label = gtk_label_new(field.label.c_str());
                gtk_label_set_wrap(GTK_LABEL(label), true);
                gtk_label_set_max_width_chars(GTK_LABEL(label), 36);
                gtk_label_set_xalign(GTK_LABEL(label), 0);
                gtk_check_button_set_child(GTK_CHECK_BUTTON(input), label);
                gtk_check_button_set_active(GTK_CHECK_BUTTON(input), field.value == "1");
                app->setup_inputs[field.id] = input;
                gtk_box_append(GTK_BOX(cell), input);
                continue;
            }
            auto *label = gtk_label_new(field.label.c_str());
            gtk_label_set_wrap(GTK_LABEL(label), true);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_box_append(GTK_BOX(cell), label);
            auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *input;
            if (!field.choices.empty()) {
                auto *model = gtk_string_list_new(nullptr);
                guint selected = 0;
                for (guint i = 0; i < field.choices.size(); ++i) {
                    gtk_string_list_append(model, field.choices[i].second.c_str());
                    if (field.choices[i].first == field.value)
                        selected = i;
                }
                input = gtk_drop_down_new(G_LIST_MODEL(model), nullptr);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(input), selected);
            } else {
                input = gtk_entry_new();
                gtk_entry_set_visibility(GTK_ENTRY(input), !field.secret);
                gtk_editable_set_text(GTK_EDITABLE(input), field.value.c_str());
            }
            gtk_widget_set_hexpand(input, true);
            gtk_box_append(GTK_BOX(row), input);
            app->setup_inputs[field.id] = input;
            if (field.file) {
                auto *browse = gtk_button_new_from_icon_name("document-open-symbolic");
                g_object_set_data(G_OBJECT(browse), "entry", input);
                g_signal_connect(browse, "clicked", G_CALLBACK(choose_setup_file), nullptr);
                gtk_box_append(GTK_BOX(row), browse);
            }
            gtk_box_append(GTK_BOX(cell), row);
        }
        app->setup_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        for (const auto &action : view.actions) {
            auto *widget = button(action.label.c_str(), G_CALLBACK(setup_action), app);
            g_object_set_data_full(G_OBJECT(widget), "action", g_strdup(action.id.c_str()), g_free);
            gtk_widget_set_hexpand(widget, true);
            gtk_label_set_wrap(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(widget))), true);
            if (action.id == view.actions.front().id)
                gtk_widget_add_css_class(widget, "suggested-action");
            gtk_box_append(GTK_BOX(app->setup_actions), widget);
        }
        gtk_box_append(GTK_BOX(app->setup_box), app->setup_actions);
        auto *log = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(log), false);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log), GTK_WRAP_WORD_CHAR);
        app->setup_log = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log));
        auto *scroll = gtk_scrolled_window_new();
        app->setup_log_scroll = scroll;
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), log);
        gtk_widget_set_size_request(scroll, -1, 110);
        gtk_box_append(GTK_BOX(app->setup_box), scroll);
        app->setup_continue = button(
            cm::tr("action.txt_ready").c_str(),
            G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<App *>(p)->wizard->next_challenge(); }),
            app);
        app->setup_cancel = button(
            cm::tr("action.cancel").c_str(),
            G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<App *>(p)->wizard->cancel(); }), app);
        gtk_box_append(GTK_BOX(app->setup_box), app->setup_continue);
        gtk_box_append(GTK_BOX(app->setup_box), app->setup_cancel);
        app->setup_revision = view.revision;
        if (!view.fields.empty())
            gtk_widget_grab_focus(app->setup_inputs.at(view.fields.front().id));
    }
    gtk_widget_set_sensitive(app->setup_fields, !view.busy);
    gtk_widget_set_sensitive(app->setup_actions, !view.busy);
    gtk_widget_set_visible(app->setup_continue, view.busy && view.manual);
    gtk_widget_set_visible(app->setup_cancel, view.busy);
    gtk_widget_set_visible(app->setup_log_scroll, view.busy || !view.log.empty());
    gtk_text_buffer_set_text(app->setup_log, view.log.c_str(), -1);
}
void activate(GtkApplication *application, gpointer data) {
    auto *app = static_cast<App *>(data);
    if (app->window) {
        gtk_window_present(app->window);
        return;
    }
    app->application = application;
    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(app->window, "CloudieMail");
    gtk_window_set_default_size(app->window, 1060, 780);
    auto *header = gtk_header_bar_new();
    gtk_window_set_titlebar(app->window, header);
    auto *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(brand), gtk_image_new_from_icon_name("mail-unread-symbolic"));
    gtk_box_append(GTK_BOX(brand), gtk_label_new("CloudieMail"));
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), brand);
    const char *language_names[] = {"Português (Brasil)", "English", "日本語", "한국어", "繁體中文", nullptr};
    auto *languages = gtk_drop_down_new_from_strings(language_names);
    const std::vector<std::string> locales = {"pt-BR", "en", "ja", "ko", "zh-TW"};
    auto selected = std::find(locales.begin(), locales.end(), cm::current_language());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(languages), static_cast<guint>(selected - locales.begin()));
    g_signal_connect(languages, "notify::selected",
                     G_CALLBACK((+[](GObject *widget, GParamSpec *, gpointer p) {
                         auto *a = static_cast<App *>(p);
                         if (a->server || a->wizard->view().busy) {
                             set_status(a, cm::tr("status.stop_first"));
                             return;
                         }
                         const char *codes[] = {"pt-BR", "en", "ja", "ko", "zh-TW"};
                         try {
                             a->wizard->language(codes[gtk_drop_down_get_selected(GTK_DROP_DOWN(widget))]);
                             refresh_setup(a);
                         } catch (const std::exception &e) {
                             set_status(a, e.what());
                         }
                     })),
                     app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), languages);
    auto *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(app->window, root);
    app->navigation = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(app->navigation, 220, -1);
    gtk_widget_add_css_class(app->navigation, "sidebar");
    for (auto set : {gtk_widget_set_margin_top, gtk_widget_set_margin_bottom, gtk_widget_set_margin_start,
                     gtk_widget_set_margin_end})
        set(app->navigation, 16);
    gtk_box_append(GTK_BOX(root), app->navigation);
    auto translated_button = [&](const char *key, const char *glyph, GCallback callback) {
        auto *b = button("", callback, app);
        auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name(glyph));
        auto *label = gtk_label_new(cm::tr(key).c_str());
        gtk_label_set_wrap(GTK_LABEL(label), true);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        app->translations.emplace_back(label, key);
        gtk_box_append(GTK_BOX(row), label);
        gtk_button_set_child(GTK_BUTTON(b), row);
        return b;
    };
    for (const auto &[action, key, glyph] : std::vector<std::tuple<const char *, const char *, const char *>>{
             {"done", "nav.overview", "view-grid-symbolic"},
             {"back", "nav.network", "network-wired-symbolic"},
             {"dns", "nav.dns", "network-server-symbolic"},
             {"certificate", "nav.certificates", "channel-secure-symbolic"},
             {"accounts", "nav.accounts", "system-users-symbolic"},
             {"preferences", "nav.preferences", "preferences-system-symbolic"}}) {
        auto *b = translated_button(
            key, glyph, G_CALLBACK(+[](GtkButton *button, gpointer p) {
                auto *a = static_cast<App *>(p);
                if (a->server) {
                    set_status(a, cm::tr("status.stop_first"));
                    return;
                }
                auto action = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "action"));
                if (std::string(action) != "done" && a->wizard->view().step != cm::SetupStep::Ready &&
                    a->config.domain == "example.test")
                    return;
                a->wizard->perform(action, {});
                refresh_setup(a);
            }));
        g_object_set_data_full(G_OBJECT(b), "action", g_strdup(action), g_free);
        gtk_widget_add_css_class(b, "flat");
        gtk_box_append(GTK_BOX(app->navigation), b);
    }
    auto *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, true);
    gtk_box_append(GTK_BOX(app->navigation), spacer);
    gtk_box_append(GTK_BOX(app->navigation),
                   translated_button(
                       "action.exit", "application-exit-symbolic", G_CALLBACK(+[](GtkButton *, gpointer p) {
                           g_application_quit(G_APPLICATION(static_cast<App *>(p)->application));
                       })));
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    app->stack = gtk_stack_new();
    gtk_widget_set_hexpand(app->stack, true);
    gtk_widget_set_vexpand(app->stack, true);
    gtk_box_append(GTK_BOX(root), app->stack);
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    for (auto set : {gtk_widget_set_margin_top, gtk_widget_set_margin_bottom, gtk_widget_set_margin_start,
                     gtk_widget_set_margin_end})
        set(box, 28);
    gtk_stack_add_named(GTK_STACK(app->stack), box, "dashboard");
    app->setup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    for (auto set : {gtk_widget_set_margin_top, gtk_widget_set_margin_bottom, gtk_widget_set_margin_start,
                     gtk_widget_set_margin_end})
        set(app->setup_box, 24);
    gtk_stack_add_named(GTK_STACK(app->stack), app->setup_box, "setup");
    auto *title = gtk_label_new(cm::tr("nav.overview").c_str());
    app->translations.emplace_back(title, "nav.overview");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(box), title);
    app->subtitle = GTK_LABEL(gtk_label_new(app->config.domain.c_str()));
    gtk_label_set_xalign(app->subtitle, 0);
    gtk_widget_add_css_class(GTK_WIDGET(app->subtitle), "dim-label");
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->subtitle));
    app->status = GTK_LABEL(gtk_label_new(cm::tr("status.stopped").c_str()));
    gtk_label_set_wrap(app->status, true);
    gtk_label_set_xalign(app->status, 0);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->status));
    auto *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    app->start_button = translated_button("action.start", "media-playback-start-symbolic", G_CALLBACK(start));
    gtk_widget_add_css_class(app->start_button, "suggested-action");
    gtk_box_append(GTK_BOX(controls), app->start_button);
    gtk_box_append(GTK_BOX(controls), translated_button("action.stop", "media-playback-stop-symbolic",
                                                        G_CALLBACK(stop_clicked)));
    gtk_box_append(GTK_BOX(controls), translated_button("action.open_webmail", "mail-unread-symbolic",
                                                        G_CALLBACK(open_webmail)));
    gtk_box_append(GTK_BOX(box), controls);
    auto *log_title = gtk_label_new(cm::tr("nav.activity").c_str());
    app->translations.emplace_back(log_title, "nav.activity");
    gtk_label_set_xalign(GTK_LABEL(log_title), 0);
    gtk_widget_add_css_class(log_title, "heading");
    gtk_box_append(GTK_BOX(box), log_title);
    auto *log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log), false);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(log), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(log), 12);
    app->activity = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log));
    auto *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), log);
    gtk_widget_set_vexpand(scroll, true);
    gtk_box_append(GTK_BOX(box), scroll);
    auto *log_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(log_controls),
                   translated_button("action.clear_log", "edit-clear-all-symbolic",
                                     G_CALLBACK(+[](GtkButton *, gpointer p) {
                                         gtk_text_buffer_set_text(static_cast<App *>(p)->activity, "", -1);
                                     })));
    gtk_box_append(
        GTK_BOX(log_controls),
        translated_button("action.copy", "edit-copy-symbolic", G_CALLBACK(+[](GtkButton *, gpointer p) {
                              auto *a = static_cast<App *>(p);
                              GtkTextIter first;
                              GtkTextIter last;
                              gtk_text_buffer_get_bounds(a->activity, &first, &last);
                              auto *value = gtk_text_buffer_get_text(a->activity, &first, &last, false);
                              gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(a->window)), value);
                              g_free(value);
                          })));
    gtk_box_append(GTK_BOX(box), log_controls);
    g_signal_connect(app->window, "close-request", G_CALLBACK(close_window), app);
    setup_tray(app);
    refresh_setup(app);
    g_timeout_add(200, update, app);
    gtk_window_present(app->window);
}
} // namespace
int main(int argc, char **argv) {
    umask(0077);
    std::signal(SIGPIPE, SIG_IGN);
    App app;
    app.path = argc > 1 ? argv[1] : "config/local.json";
    try {
        app.wizard = std::make_unique<cm::Wizard>(app.path);
        if (std::filesystem::exists(app.path))
            app.config = cm::Config::load(app.path);
    } catch (const std::exception &e) {
        g_printerr("%s\n", e.what());
        return 1;
    }
    std::signal(SIGTERM, interrupt_gui);
    std::signal(SIGINT, interrupt_gui);
    auto *application = gtk_application_new("com.cloudie.CloudieMail", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    int result = g_application_run(G_APPLICATION(application), 1, argv);
    stop(&app);
    if (app.watch)
        g_bus_unwatch_name(app.watch);
    if (app.bus) {
        if (app.registration)
            g_dbus_connection_unregister_object(app.bus, app.registration);
        g_object_unref(app.bus);
    }
    g_object_unref(application);
    return result;
}
