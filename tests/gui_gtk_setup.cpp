#define main cloudiemail_gui_entrypoint
#include "../src/platform/linux/gui_gtk.cpp"
#undef main
#include <iostream>

namespace {
void pump() {
    while (g_main_context_iteration(nullptr, false)) {
    }
}
void wait_step(App &app, cm::SetupStep expected) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        pump();
        refresh_setup(&app);
        auto view = app.wizard->view();
        if (!view.busy && view.step == expected && app.setup_revision == view.revision)
            return;
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error(view.log);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
void click_action(App &app, const std::string &action) {
    for (auto *widget = gtk_widget_get_first_child(app.setup_actions); widget;
         widget = gtk_widget_get_next_sibling(widget)) {
        auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "action"));
        if (id && action == id) {
            gtk_widget_grab_focus(widget);
            pump();
            g_signal_emit_by_name(widget, "clicked");
            return;
        }
    }
    throw std::runtime_error("Action button not found");
}
void snapshot(App &app, const std::filesystem::path &path) {
    for (int i = 0; i < 20; ++i) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto *paintable = gtk_widget_paintable_new(GTK_WIDGET(app.window));
    auto *snapshot = gtk_snapshot_new();
    auto width = gtk_widget_get_width(GTK_WIDGET(app.window)),
         height = gtk_widget_get_height(GTK_WIDGET(app.window));
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    auto *node = gtk_snapshot_free_to_node(snapshot);
    if (!node)
        throw std::runtime_error("No GTK render node");
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width), static_cast<float>(height));
    auto *texture =
        gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(app.window)), node, &bounds);
    if (!gdk_texture_save_to_png(texture, path.c_str()))
        throw std::runtime_error("Screenshot failed");
    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 5)
            throw std::runtime_error("Expected config, certificate, key, screenshot-directory");
        umask(0077);
        std::signal(SIGPIPE, SIG_IGN);
        cm::crypto_init();
        App app;
        app.path = argv[1];
        app.wizard = std::make_unique<cm::Wizard>(app.path);
        auto *application =
            gtk_application_new("com.cloudie.CloudieMail.SetupTest", G_APPLICATION_NON_UNIQUE);
        GError *error = nullptr;
        if (!g_application_register(G_APPLICATION(application), nullptr, &error))
            throw std::runtime_error(error->message);
        activate(application, &app);
        wait_step(app, cm::SetupStep::Domain);
        if (gtk_widget_get_sensitive(app.start_button))
            throw std::runtime_error("Start enabled before setup");
        std::filesystem::create_directories(argv[4]);
        snapshot(app, std::filesystem::path(argv[4]) / "setup-domain.png");
        gtk_editable_set_text(GTK_EDITABLE(app.setup_inputs.at("domain")), "example.test");
        gtk_editable_set_text(GTK_EDITABLE(app.setup_inputs.at("ipv4")), "192.0.2.10");
        click_action(app, "domain");
        wait_step(app, cm::SetupStep::Dns);
        click_action(app, "manual");
        wait_step(app, cm::SetupStep::Certificate);
        snapshot(app, std::filesystem::path(argv[4]) / "setup-certificate.png");
        gtk_editable_set_text(GTK_EDITABLE(app.setup_inputs.at("cert")), argv[2]);
        gtk_editable_set_text(GTK_EDITABLE(app.setup_inputs.at("key")), argv[3]);
        click_action(app, "import");
        wait_step(app, cm::SetupStep::Account);
        if (gtk_widget_get_sensitive(app.start_button))
            throw std::runtime_error("Start enabled without an account");
        gtk_editable_set_text(GTK_EDITABLE(app.setup_inputs.at("password")), "gui onboarding test password!");
        gtk_editable_set_text(GTK_EDITABLE(app.setup_inputs.at("confirmation")),
                              "gui onboarding test password!");
        click_action(app, "account");
        wait_step(app, cm::SetupStep::Ready);
        if (!gtk_widget_get_sensitive(app.start_button) || app.server)
            throw std::runtime_error("Setup auto-started or did not unlock server");
        snapshot(app, std::filesystem::path(argv[4]) / "setup-ready.png");
        start(nullptr, &app);
        auto started_by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (app.server && !app.server->running() && std::chrono::steady_clock::now() < started_by) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!app.server || !app.server->running())
            throw std::runtime_error("Screenshot server did not start");
        update(&app);
        snapshot(app, std::filesystem::path(argv[4]) / "linux-overview.png");
        stop_clicked(nullptr, &app);
        app.wizard->perform("preferences", {});
        wait_step(app, cm::SetupStep::Preferences);
        snapshot(app, std::filesystem::path(argv[4]) / "linux-settings.png");
        if (app.watch)
            g_bus_unwatch_name(app.watch);
        if (app.bus) {
            if (app.registration)
                g_dbus_connection_unregister_object(app.bus, app.registration);
            g_object_unref(app.bus);
        }
        gtk_window_destroy(app.window);
        g_object_unref(application);
        std::cout << "GTK4 wizard: domain -> DNS -> certificate -> first account -> ready, server stopped\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
