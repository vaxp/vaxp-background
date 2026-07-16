#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <string.h>

#define WALLPAPER_DIR          "/usr/share/backgrounds"

static GtkWidget *main_window = NULL;

static char *get_vaxp_config_path(const char *filename) {
    return g_build_filename(g_get_user_config_dir(), "vaxp", "desktop", filename, NULL);
}

static char *get_vaxp_main_config_path(void) {
    return get_vaxp_config_path("desktop.vaxp");
}

static char *get_vaxp_cache_path(const char *filename) {
    return g_build_filename(g_get_user_cache_dir(), "vaxp-thumbnails", filename, NULL);
}

static void ensure_config_dir(void) {
    char *dir = g_build_filename(g_get_user_config_dir(), "vaxp", "desktop", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
}

/* ── File type helpers ─────────────────────────────────────────── */

static gboolean is_image_file(const char *name) {
    const char *exts[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif",
                           ".webp", ".tiff", ".svg", NULL };
    char *lower = g_ascii_strdown(name, -1);
    gboolean ok = FALSE;
    for (int i = 0; exts[i]; i++)
        if (g_str_has_suffix(lower, exts[i])) { ok = TRUE; break; }
    g_free(lower);
    return ok;
}

static gboolean is_video_file_ext(const char *name) {
    const char *exts[] = { ".mp4", ".mkv", ".webm", ".avi", ".mov",
                           ".flv", ".wmv", ".m4v", ".ogv", ".ts",
                           ".m2ts", ".mpg", ".mpeg", ".3gp", NULL };
    char *lower = g_ascii_strdown(name, -1);
    gboolean ok = FALSE;
    for (int i = 0; exts[i]; i++)
        if (g_str_has_suffix(lower, exts[i])) { ok = TRUE; break; }
    g_free(lower);
    return ok;
}

/* ── Config helpers ────────────────────────────────────────────── */

static void set_wallpaper(const char *path) {
    ensure_config_dir();
    char *main_config = get_vaxp_main_config_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, main_config, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string(kf, "Desktop", "Wallpaper", path);
    g_key_file_save_to_file(kf, main_config, NULL);
    g_key_file_free(kf);
    g_free(main_config);
}

static void on_anim_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    int id = gtk_combo_box_get_active(combo);
    ensure_config_dir();
    char *main_config = get_vaxp_main_config_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, main_config, G_KEY_FILE_NONE, NULL);
    g_key_file_set_integer(kf, "Desktop", "WallpaperAnim", id);
    g_key_file_save_to_file(kf, main_config, NULL);
    g_key_file_free(kf);
    g_free(main_config);
}

static int get_saved_anim(void) {
    int id = 0;
    char *main_config = get_vaxp_main_config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, main_config, G_KEY_FILE_NONE, NULL)) {
        GError *err = NULL;
        int a = g_key_file_get_integer(kf, "Desktop", "WallpaperAnim", &err);
        if (!err) id = a;
        else g_error_free(err);
    }
    g_key_file_free(kf);
    g_free(main_config);
    if (id < 0 || id > 9) id = 0;
    return id;
}

static int get_saved_volume(void) {
    int vol = 0;
    char *main_config = get_vaxp_main_config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, main_config, G_KEY_FILE_NONE, NULL)) {
        GError *err = NULL;
        int v = g_key_file_get_integer(kf, "Desktop", "VideoVolume", &err);
        if (!err) vol = v;
        else g_error_free(err);
    }
    g_key_file_free(kf);
    g_free(main_config);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return vol;
}

/* ── Image loader (lazy, idle-based) ───────────────────────────── */

typedef struct { GtkWidget *flow; char *dir_path; GDir *dir; } DirLoader;

static gboolean load_next_image(gpointer user_data) {
    DirLoader *loader = user_data;
    const char *fname;

    for (int i = 0; i < 3; i++) {
        fname = g_dir_read_name(loader->dir);
        if (!fname) {
            g_dir_close(loader->dir);
            g_free(loader->dir_path);
            g_free(loader);
            return G_SOURCE_REMOVE;
        }
        if (!is_image_file(fname)) continue;

        char *full_path = g_strdup_printf("%s/%s", loader->dir_path, fname);
        GdkPixbuf *thumb = gdk_pixbuf_new_from_file_at_scale(full_path, 180, 110, FALSE, NULL);
        GtkWidget *img = thumb
            ? gtk_image_new_from_pixbuf(thumb)
            : gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_DIALOG);
        if (thumb) g_object_unref(thumb);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_pack_start(GTK_BOX(vbox), img, FALSE, FALSE, 0);
        GtkWidget *lbl = gtk_label_new(fname);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 22);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);
        g_object_set_data_full(G_OBJECT(vbox), "wallpaper-path", full_path, g_free);
        gtk_flow_box_insert(GTK_FLOW_BOX(loader->flow), vbox, -1);
        gtk_widget_show_all(vbox);
    }
    return G_SOURCE_CONTINUE;
}

static void add_images_from_dir(const char *dir_path, GtkWidget *flow) {
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return;
    DirLoader *loader = g_new(DirLoader, 1);
    loader->flow     = flow;
    loader->dir_path = g_strdup(dir_path);
    loader->dir      = dir;
    g_idle_add(load_next_image, loader);
}

/* ── Video loader (synchronous — less files expected) ──────────── */

static gboolean load_next_video(gpointer user_data) {
    DirLoader *loader = user_data;
    const char *fname;

    /* Only 1 video per tick since ffmpegthumbnailer takes time */
    fname = g_dir_read_name(loader->dir);
    if (!fname) {
        g_dir_close(loader->dir);
        g_free(loader->dir_path);
        g_free(loader);
        return G_SOURCE_REMOVE;
    }
    if (!is_video_file_ext(fname)) return G_SOURCE_CONTINUE;

    char *full_path = g_strdup_printf("%s/%s", loader->dir_path, fname);

    char *cache_dir = g_build_filename(g_get_user_cache_dir(), "vaxp-thumbnails", NULL);
    g_mkdir_with_parents(cache_dir, 0755);
    g_free(cache_dir);
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_MD5, full_path, -1);
    char *thumb_name = g_strdup_printf("%s.png", hash);
    char *thumb_path = get_vaxp_cache_path(thumb_name);
    g_free(thumb_name);
    g_free(hash);

    if (!g_file_test(thumb_path, G_FILE_TEST_EXISTS)) {
        char *cmd = g_strdup_printf("ffmpegthumbnailer -i \"%s\" -o \"%s\" -s 180 -t 5%% -q 5 -c png >/dev/null 2>&1",
                                    full_path, thumb_path);
        system(cmd);
        g_free(cmd);
    }

    GdkPixbuf *thumb = gdk_pixbuf_new_from_file_at_scale(thumb_path, 180, 110, FALSE, NULL);
    GtkWidget *img = thumb
        ? gtk_image_new_from_pixbuf(thumb)
        : gtk_image_new_from_icon_name("video-x-generic", GTK_ICON_SIZE_DIALOG);
    
    if (thumb) g_object_unref(thumb);
    if (!thumb) gtk_image_set_pixel_size(GTK_IMAGE(img), 80);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), img, FALSE, FALSE, 0);
    GtkWidget *lbl = gtk_label_new(fname);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 22);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);
    
    g_object_set_data_full(G_OBJECT(vbox), "wallpaper-path", full_path, g_free);
    gtk_flow_box_insert(GTK_FLOW_BOX(loader->flow), vbox, -1);
    gtk_widget_show_all(vbox);

    g_free(thumb_path);
    return G_SOURCE_CONTINUE;
}

static void add_videos_from_dir(const char *dir_path, GtkWidget *flow) {
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return;
    DirLoader *loader = g_new(DirLoader, 1);
    loader->flow     = flow;
    loader->dir_path = g_strdup(dir_path);
    loader->dir      = dir;
    g_idle_add(load_next_video, loader);
}

/* ── Browse-folder button ──────────────────────────────────────── */

typedef struct { GtkWidget *img_flow; GtkWidget *vid_flow; } BrowseData;

static void on_browse_folder_clicked(GtkButton *btn, gpointer user_data) {
    BrowseData *bd = user_data;
    (void)btn;

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Select Folder",
        GTK_WINDOW(main_window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(chooser), 600, 400);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (folder) {
            add_images_from_dir(folder, bd->img_flow);
            add_videos_from_dir(folder, bd->vid_flow);

            char **existing_dirs = NULL;
            gsize num_dirs = 0;
            char *main_config = get_vaxp_main_config_path();
            GKeyFile *kf = g_key_file_new();
            g_key_file_load_from_file(kf, main_config, G_KEY_FILE_NONE, NULL);
            existing_dirs = g_key_file_get_string_list(kf, "Desktop", "WallpaperDirs", &num_dirs, NULL);
            
            gboolean already = FALSE;
            for (gsize i = 0; i < num_dirs; i++) {
                if (g_strcmp0(existing_dirs[i], folder) == 0) {
                    already = TRUE;
                    break;
                }
            }
            if (!already) {
                char **new_dirs = g_new0(char *, num_dirs + 2);
                for (gsize i = 0; i < num_dirs; i++) new_dirs[i] = existing_dirs[i];
                new_dirs[num_dirs] = folder;
                ensure_config_dir();
                g_key_file_set_string_list(kf, "Desktop", "WallpaperDirs", (const gchar * const *)new_dirs, num_dirs + 1);
                g_key_file_save_to_file(kf, main_config, NULL);
                g_free(new_dirs);
            }
            g_strfreev(existing_dirs);
            g_key_file_free(kf);
            g_free(main_config);
            g_free(folder);
        }
    }
    gtk_widget_destroy(chooser);
}

/* ── Selection callbacks ───────────────────────────────────────── */

static void on_wallpaper_selected(GtkFlowBox *box, GtkFlowBoxChild *child,
                                  gpointer user_data) {
    (void)box; (void)user_data;
    GtkWidget *vbox = gtk_bin_get_child(GTK_BIN(child));
    const char *path = g_object_get_data(G_OBJECT(vbox), "wallpaper-path");
    if (path) set_wallpaper(path);
}

/* Volume scale changed → persist */
static void on_volume_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    int vol = (int)gtk_range_get_value(range);
    ensure_config_dir();
    char *main_config = get_vaxp_main_config_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, main_config, G_KEY_FILE_NONE, NULL);
    g_key_file_set_integer(kf, "Desktop", "VideoVolume", vol);
    g_key_file_save_to_file(kf, main_config, NULL);
    g_key_file_free(kf);
    g_free(main_config);
}

/* ── main ──────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    main_window = gtk_dialog_new_with_buttons(
        "Change Wallpaper",
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply",  GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 860, 560);

    GdkScreen *screen = gtk_widget_get_screen(main_window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual && gdk_screen_is_composited(screen)) {
        gtk_widget_set_visual(main_window, visual);
        gtk_widget_set_app_paintable(main_window, TRUE);
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window, window.background, dialog {"
        "  background-color: rgba(0,0,0,0.3);"
        "  background-image: none; }"
        "notebook, notebook > header, notebook stack, scrolledwindow, viewport, flowbox {"
        "  background-color: transparent;"
        "  background-image: none; border: none; box-shadow: none; }"
        "label { color: white; }"
        "notebook tab { color: white; }"
        "notebook tab:checked { color: #aaddff; font-weight: bold; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(main_window));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);

    /* ── Top toolbar ── */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_bottom(toolbar, 6);
    gtk_box_pack_start(GTK_BOX(content), toolbar, FALSE, FALSE, 0);

    GtkWidget *anim_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "1. Sliding Doors");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "2. Circle Reveal");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "3. Smooth Crossfade");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "4. Wipe Right");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "5. Zoom Out & Fade");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "6. Blinds");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "7. Swipe Up");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "8. Grid/Mosaic");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "9. Diagonal Wipe");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(anim_combo), "10. Spin & Fade");
    gtk_combo_box_set_active(GTK_COMBO_BOX(anim_combo), get_saved_anim());
    g_signal_connect(anim_combo, "changed", G_CALLBACK(on_anim_changed), NULL);

    GtkWidget *anim_lbl = gtk_label_new("Image Transition:");
    gtk_box_pack_start(GTK_BOX(toolbar), anim_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), anim_combo, FALSE, FALSE, 0);

    GtkWidget *browse_btn = gtk_button_new_with_label("📁 Add Folder");
    gtk_box_pack_end(GTK_BOX(toolbar), browse_btn, FALSE, FALSE, 0);

    /* ── Notebook with two tabs ── */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);

    /* ── Tab 1: Images ── */
    GtkWidget *img_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(img_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *img_flow = gtk_flow_box_new();
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(img_flow), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(img_flow), 2);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(img_flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(img_flow), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(img_flow), 8);
    gtk_widget_set_margin_start(img_flow, 4);
    gtk_widget_set_margin_end(img_flow, 4);
    gtk_container_add(GTK_CONTAINER(img_scroll), img_flow);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), img_scroll,
                             gtk_label_new("🖼  Images"));

    /* ── Tab 2: Videos ── */
    GtkWidget *vid_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vid_vbox), 6);

    /* Volume row */
    GtkWidget *vol_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *vol_lbl = gtk_label_new("🔊 Volume:");
    GtkWidget *vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                    0, 100, 1);
    gtk_range_set_value(GTK_RANGE(vol_scale), get_saved_volume());
    gtk_widget_set_hexpand(vol_scale, TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(vol_scale), GTK_POS_RIGHT);
    g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_volume_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vol_row), vol_lbl,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vol_row), vol_scale, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(vid_vbox), vol_row, FALSE, FALSE, 0);

    GtkWidget *vid_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(vid_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(vid_scroll, TRUE);

    GtkWidget *vid_flow = gtk_flow_box_new();
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(vid_flow), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(vid_flow), 2);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(vid_flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(vid_flow), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(vid_flow), 8);
    gtk_widget_set_margin_start(vid_flow, 4);
    gtk_widget_set_margin_end(vid_flow, 4);
    gtk_container_add(GTK_CONTAINER(vid_scroll), vid_flow);
    gtk_box_pack_start(GTK_BOX(vid_vbox), vid_scroll, TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vid_vbox,
                             gtk_label_new("🎬 Videos"));

    /* ── Wire browse button to both flows ── */
    BrowseData *bd = g_new(BrowseData, 1);
    bd->img_flow = img_flow;
    bd->vid_flow = vid_flow;
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_browse_folder_clicked), bd);

    /* ── Selection signals ── */
    g_signal_connect(img_flow, "child-activated", G_CALLBACK(on_wallpaper_selected), NULL);
    g_signal_connect(vid_flow, "child-activated", G_CALLBACK(on_wallpaper_selected), NULL);

    /* ── Populate images ── */
    add_images_from_dir(WALLPAPER_DIR, img_flow);
    {
        char *main_config = get_vaxp_main_config_path();
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, main_config, G_KEY_FILE_NONE, NULL)) {
            gsize num_dirs = 0;
            char **dirs = g_key_file_get_string_list(kf, "Desktop", "WallpaperDirs", &num_dirs, NULL);
            if (dirs) {
                for (gsize i = 0; i < num_dirs; i++) {
                    if (strlen(dirs[i]) > 1 && dirs[i][0] == '/') {
                        add_images_from_dir(dirs[i], img_flow);
                        add_videos_from_dir(dirs[i], vid_flow);
                    }
                }
                g_strfreev(dirs);
            }
        }
        g_key_file_free(kf);
        g_free(main_config);
    }

    /* Also scan default video dirs */
    const char *home = g_get_home_dir();
    char *vid_home = g_strdup_printf("%s/Videos", home);
    add_videos_from_dir(vid_home, vid_flow);
    g_free(vid_home);

    gtk_widget_show_all(main_window);

    if (gtk_dialog_run(GTK_DIALOG(main_window)) == GTK_RESPONSE_ACCEPT) {
        /* Check which tab is active */
        int tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
        GtkWidget *active_flow = (tab == 1) ? vid_flow : img_flow;
        GList *selected = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(active_flow));
        if (selected) {
            GtkFlowBoxChild *child = GTK_FLOW_BOX_CHILD(selected->data);
            GtkWidget *box = gtk_bin_get_child(GTK_BIN(child));
            const char *path = g_object_get_data(G_OBJECT(box), "wallpaper-path");
            if (path) set_wallpaper(path);
            g_list_free(selected);
        }
    }

    g_free(bd);
    gtk_widget_destroy(main_window);
    return 0;
}
