#include "tray.h"

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <unistd.h>

#include "constants.h"
#include "utils.h"

#define SNI_IFACE   "org.kde.StatusNotifierItem"
#define MENU_IFACE  "com.canonical.dbusmenu"
#define SNI_PATH    "/StatusNotifierItem"
#define MENU_PATH   "/MenuBar"
#define WATCHER_BUS "org.kde.StatusNotifierWatcher"
#define TRAY_ICON_ACTIVE   "dict-tray"
#define TRAY_ICON_INACTIVE "dict-tray-inactive"

static const char *INTROSPECTION_XML =
  "<node>"
  "  <interface name='" SNI_IFACE "'>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='WindowId' type='i' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='IconPixmap' type='a(iiay)' access='read'/>"
  "    <property name='OverlayIconName' type='s' access='read'/>"
  "    <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
  "    <property name='AttentionIconName' type='s' access='read'/>"
  "    <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
  "    <property name='AttentionMovieName' type='s' access='read'/>"
  "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "    <property name='IconThemePath' type='s' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <method name='Activate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='SecondaryActivate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='ContextMenu'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='Scroll'>"
  "      <arg type='i' name='delta' direction='in'/>"
  "      <arg type='s' name='orientation' direction='in'/>"
  "    </method>"
  "    <signal name='NewTitle'/>"
  "    <signal name='NewIcon'/>"
  "    <signal name='NewToolTip'/>"
  "    <signal name='NewStatus'>"
  "      <arg type='s' name='status'/>"
  "    </signal>"
  "  </interface>"
  "  <interface name='" MENU_IFACE "'>"
  "    <property name='Version' type='u' access='read'/>"
  "    <property name='TextDirection' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconThemePath' type='as' access='read'/>"
  "    <method name='GetLayout'>"
  "      <arg type='i' name='parentId' direction='in'/>"
  "      <arg type='i' name='recursionDepth' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='u' name='revision' direction='out'/>"
  "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
  "    </method>"
  "    <method name='GetGroupProperties'>"
  "      <arg type='ai' name='ids' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
  "    </method>"
  "    <method name='GetProperty'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='s' name='name' direction='in'/>"
  "      <arg type='v' name='value' direction='out'/>"
  "    </method>"
  "    <method name='Event'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='s' name='eventId' direction='in'/>"
  "      <arg type='v' name='data' direction='in'/>"
  "      <arg type='u' name='timestamp' direction='in'/>"
  "    </method>"
  "    <method name='AboutToShow'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='b' name='needUpdate' direction='out'/>"
  "    </method>"
  "    <signal name='ItemsPropertiesUpdated'>"
  "      <arg type='a(ia{sv})' name='updatedProps'/>"
  "      <arg type='a(ias)' name='removedProps'/>"
  "    </signal>"
  "    <signal name='LayoutUpdated'>"
  "      <arg type='u' name='revision'/>"
  "      <arg type='i' name='parent'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

/* Menu item ids (flat, static menu — nothing here is ever added/removed). */
enum {
  MENU_ID_ROOT = 0,
  MENU_ID_OPEN = 1,
  MENU_ID_SEP1 = 2,
  MENU_ID_CLIPBOARD = 3,
  MENU_ID_BACKGROUND = 4,
  MENU_ID_SEP2 = 5,
  MENU_ID_QUIT = 6,
};

struct _WbTray {
  WbTrayCallbacks cb;

  GDBusNodeInfo       *node_info;
  GDBusInterfaceInfo  *item_iface;
  GDBusInterfaceInfo  *menu_iface;

  GDBusConnection *conn;       /* borrowed, valid once bus_acquired fires */
  char            *service_name;
  guint            own_name_id;
  guint            watcher_watch_id;
  guint            item_reg_id;
  guint            menu_reg_id;
  gboolean         registered_with_watcher;
  guint            layout_revision;
  char            *icon_dir;   /* where the bundled tray SVGs were written */
};

/* ---------------------------------------------------------------------- */
/* StatusNotifierItem object                                              */
/* ---------------------------------------------------------------------- */

static void
item_method_call (GDBusConnection *connection, const char *sender,
                   const char *object_path, const char *interface_name,
                   const char *method_name, GVariant *parameters,
                   GDBusMethodInvocation *invocation, gpointer user_data)
{
  WbTray *self = user_data;

  if (g_strcmp0 (method_name, "Activate") == 0) {
    /* Left click arms/disarms clipboard lookup. It deliberately does NOT
       open the mini window: that only appears when something is copied
       while lookup is on. */
    if (self->cb.on_set_clipboard_watch && self->cb.get_clipboard_watch)
      self->cb.on_set_clipboard_watch (self->cb.user_data,
                                       !self->cb.get_clipboard_watch (self->cb.user_data));
    g_dbus_method_invocation_return_value (invocation, NULL);
    return;
  }

  if (g_strcmp0 (method_name, "SecondaryActivate") == 0) {
    if (self->cb.on_open_window)
      self->cb.on_open_window (self->cb.user_data);
    g_dbus_method_invocation_return_value (invocation, NULL);
    return;
  }

  if (g_strcmp0 (method_name, "ContextMenu") == 0 ||
      g_strcmp0 (method_name, "Scroll") == 0) {
    /* The Menu object handles context menus for hosts that support
       com.canonical.dbusmenu (waybar included); nothing else to do here. */
    g_dbus_method_invocation_return_value (invocation, NULL);
    return;
  }

  g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                         "Unknown method %s", method_name);
}

static GVariant *
empty_pixmap_array (void)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(iiay)"));
  return g_variant_builder_end (&b);
}

static GVariant *
item_get_property (GDBusConnection *connection, const char *sender,
                    const char *object_path, const char *interface_name,
                    const char *property_name, GError **error, gpointer user_data)
{
  WbTray *self = user_data;
  gboolean active = self->cb.get_clipboard_watch
    ? self->cb.get_clipboard_watch (self->cb.user_data) : FALSE;

  if (g_strcmp0 (property_name, "Category") == 0)
    return g_variant_new_string ("ApplicationStatus");
  if (g_strcmp0 (property_name, "Id") == 0)
    return g_variant_new_string ("dev.silvanus.dictionary");
  if (g_strcmp0 (property_name, "Title") == 0)
    return g_variant_new_string (_("Dictionary"));
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string ("Active");
  if (g_strcmp0 (property_name, "WindowId") == 0)
    return g_variant_new_int32 (0);
  if (g_strcmp0 (property_name, "IconName") == 0)
    return g_variant_new_string (active ? TRAY_ICON_ACTIVE : TRAY_ICON_INACTIVE);
  if (g_strcmp0 (property_name, "IconPixmap") == 0 ||
      g_strcmp0 (property_name, "OverlayIconPixmap") == 0 ||
      g_strcmp0 (property_name, "AttentionIconPixmap") == 0)
    return empty_pixmap_array ();
  if (g_strcmp0 (property_name, "OverlayIconName") == 0 ||
      g_strcmp0 (property_name, "AttentionIconName") == 0 ||
      g_strcmp0 (property_name, "AttentionMovieName") == 0)
    return g_variant_new_string ("");
  if (g_strcmp0 (property_name, "ToolTip") == 0) {
    GVariantBuilder pix;
    g_variant_builder_init (&pix, G_VARIANT_TYPE ("a(iiay)"));
    return g_variant_new ("(sa(iiay)ss)", "", &pix, _("Dictionary"),
                          active ? _("Word lookup is on \xe2\x80\x94 click to turn off")
                                 : _("Word lookup is off \xe2\x80\x94 click to turn on"));
  }
  if (g_strcmp0 (property_name, "IconThemePath") == 0)
    return g_variant_new_string (self->icon_dir ? self->icon_dir : "");
  if (g_strcmp0 (property_name, "ItemIsMenu") == 0)
    return g_variant_new_boolean (FALSE);
  if (g_strcmp0 (property_name, "Menu") == 0)
    return g_variant_new_object_path (MENU_PATH);

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
              "Unknown property %s", property_name);
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* com.canonical.dbusmenu object                                          */
/* ---------------------------------------------------------------------- */

static GVariant *
leaf_item (int id, const char *type, const char *label,
          const char *toggle_type, int toggle_state)
{
  GVariantBuilder props;
  g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
  if (type)
    g_variant_builder_add (&props, "{sv}", "type", g_variant_new_string (type));
  if (label)
    g_variant_builder_add (&props, "{sv}", "label", g_variant_new_string (label));
  g_variant_builder_add (&props, "{sv}", "enabled", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&props, "{sv}", "visible", g_variant_new_boolean (TRUE));
  if (toggle_type) {
    g_variant_builder_add (&props, "{sv}", "toggle-type", g_variant_new_string (toggle_type));
    g_variant_builder_add (&props, "{sv}", "toggle-state", g_variant_new_int32 (toggle_state));
  }

  GVariantBuilder kids;
  g_variant_builder_init (&kids, G_VARIANT_TYPE ("av"));
  return g_variant_new ("(ia{sv}av)", id, &props, &kids);
}

static GVariant *
build_root_layout (WbTray *self)
{
  gboolean clip_on = self->cb.get_clipboard_watch ? self->cb.get_clipboard_watch (self->cb.user_data) : FALSE;
  gboolean bg_on   = self->cb.get_run_in_background ? self->cb.get_run_in_background (self->cb.user_data) : FALSE;

  GVariantBuilder kids;
  g_variant_builder_init (&kids, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&kids, "v", leaf_item (MENU_ID_OPEN, "standard", _("Open Dictionary"), NULL, 0));
  g_variant_builder_add (&kids, "v", leaf_item (MENU_ID_SEP1, "separator", NULL, NULL, 0));
  g_variant_builder_add (&kids, "v", leaf_item (MENU_ID_CLIPBOARD, "standard",
                                                _("Automatic Word Lookup"), "checkmark", clip_on ? 1 : 0));
  g_variant_builder_add (&kids, "v", leaf_item (MENU_ID_BACKGROUND, "standard",
                                                _("Keep Running in Background"), "checkmark", bg_on ? 1 : 0));
  g_variant_builder_add (&kids, "v", leaf_item (MENU_ID_SEP2, "separator", NULL, NULL, 0));
  g_variant_builder_add (&kids, "v", leaf_item (MENU_ID_QUIT, "standard", _("Quit Dictionary"), NULL, 0));

  GVariantBuilder props;
  g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&props, "{sv}", "children-display", g_variant_new_string ("submenu"));

  return g_variant_new ("(ia{sv}av)", MENU_ID_ROOT, &props, &kids);
}

static void
menu_method_call (GDBusConnection *connection, const char *sender,
                  const char *object_path, const char *interface_name,
                  const char *method_name, GVariant *parameters,
                  GDBusMethodInvocation *invocation, gpointer user_data)
{
  WbTray *self = user_data;

  if (g_strcmp0 (method_name, "GetLayout") == 0) {
    int parent_id = 0, depth = 0;
    g_variant_get_child (parameters, 0, "i", &parent_id);
    g_variant_get_child (parameters, 1, "i", &depth);
    /* The menu is flat and static: anything other than the root just gets
       itself back with no children, which satisfies the protocol without
       needing a lookup table keyed by id. */
    GVariant *layout = (parent_id == MENU_ID_ROOT)
      ? build_root_layout (self)
      : leaf_item (parent_id, NULL, NULL, NULL, 0);
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u@(ia{sv}av))", self->layout_revision, layout));
    return;
  }

  if (g_strcmp0 (method_name, "GetGroupProperties") == 0) {
    GVariantBuilder out;
    g_variant_builder_init (&out, G_VARIANT_TYPE ("a(ia{sv})"));
    /* Reuse the root layout's children to answer property lookups; good
       enough for a menu this small and keeps a single source of truth. */
    GVariant *root = build_root_layout (self);
    GVariant *kids = g_variant_get_child_value (root, 2);
    GVariantIter it;
    GVariant *child;
    g_variant_iter_init (&it, kids);
    while ((child = g_variant_iter_next_value (&it))) {
      GVariant *item = g_variant_get_variant (child);
      /* Pull id/props out by index rather than g_variant_get() with a NULL
         placeholder for the trailing "av" field — passing NULL there isn't
         a documented "skip this" for a plain (non-maybe) type, and this is
         exactly the kind of thing that works in testing and then segfaults
         the first time a real host calls GetGroupProperties. */
      GVariant *id_v = g_variant_get_child_value (item, 0);
      GVariant *props = g_variant_get_child_value (item, 1);
      int id = g_variant_get_int32 (id_v);
      g_variant_builder_add (&out, "(i@a{sv})", id, props);
      g_variant_unref (id_v);
      g_variant_unref (props);
      g_variant_unref (item);
      g_variant_unref (child);
    }
    g_variant_unref (kids);
    g_variant_unref (root);
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(ia{sv}))", &out));
    return;
  }

  if (g_strcmp0 (method_name, "GetProperty") == 0) {
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(v)", g_variant_new_boolean (FALSE)));
    return;
  }

  if (g_strcmp0 (method_name, "Event") == 0) {
    int id = 0;
    const char *event_id = NULL;
    g_variant_get_child (parameters, 0, "i", &id);
    g_variant_get_child (parameters, 1, "&s", &event_id);

    if (g_strcmp0 (event_id, "clicked") == 0) {
      switch (id) {
        case MENU_ID_OPEN:
          if (self->cb.on_open_window)
            self->cb.on_open_window (self->cb.user_data);
          break;
        case MENU_ID_CLIPBOARD:
          if (self->cb.on_set_clipboard_watch && self->cb.get_clipboard_watch)
            self->cb.on_set_clipboard_watch (self->cb.user_data,
              !self->cb.get_clipboard_watch (self->cb.user_data));
          break;
        case MENU_ID_BACKGROUND:
          if (self->cb.on_set_run_in_background && self->cb.get_run_in_background)
            self->cb.on_set_run_in_background (self->cb.user_data,
              !self->cb.get_run_in_background (self->cb.user_data));
          break;
        case MENU_ID_QUIT:
          if (self->cb.on_quit)
            self->cb.on_quit (self->cb.user_data);
          break;
        default:
          break;
      }
    }
    g_dbus_method_invocation_return_value (invocation, NULL);
    return;
  }

  if (g_strcmp0 (method_name, "AboutToShow") == 0) {
    /* Always TRUE: forces the host to re-fetch GetLayout right before
       showing, so the two checkmarks always reflect current settings even
       if they were changed from the Preferences window instead. */
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", TRUE));
    return;
  }

  g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                         "Unknown method %s", method_name);
}

static GVariant *
menu_get_property (GDBusConnection *connection, const char *sender,
                   const char *object_path, const char *interface_name,
                   const char *property_name, GError **error, gpointer user_data)
{
  if (g_strcmp0 (property_name, "Version") == 0)
    return g_variant_new_uint32 (3);
  if (g_strcmp0 (property_name, "TextDirection") == 0)
    return g_variant_new_string ("ltr");
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string ("normal");
  if (g_strcmp0 (property_name, "IconThemePath") == 0) {
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("as"));
    return g_variant_builder_end (&b);
  }
  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
              "Unknown property %s", property_name);
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* Registration with whatever StatusNotifierWatcher is on the bus         */
/* ---------------------------------------------------------------------- */

static void
on_register_reply (GObject *source, GAsyncResult *res, gpointer user_data)
{
  WbTray *self = user_data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);

  if (err != NULL) {
    /* This is exactly the message someone needs when the icon never shows
       up — surface it at WARNING so it's visible without --verbose, not
       buried behind INFO which is suppressed by default. */
    log_warning ("Tray: RegisterStatusNotifierItem failed (%s). Usual cause: "
                "no StatusNotifierWatcher is running — on waybar, that means "
                "the \"tray\" module isn't in your bar's modules list, or "
                "this waybar build has tray support disabled.", err->message);
    return;
  }
  self->registered_with_watcher = TRUE;
  log_warning ("Tray: registered with StatusNotifierWatcher — icon should now "
              "be visible in any SNI host that's running.");
}

static void
register_with_watcher (WbTray *self)
{
  if (self->conn == NULL || self->service_name == NULL)
    return;
  g_dbus_connection_call (self->conn, WATCHER_BUS, "/StatusNotifierWatcher", WATCHER_BUS,
                          "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", self->service_name),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                          on_register_reply, self);
}

static void
on_watcher_appeared (GDBusConnection *connection, const char *name,
                     const char *owner, gpointer user_data)
{
  register_with_watcher (user_data);
}

static void
on_watcher_vanished (GDBusConnection *connection, const char *name, gpointer user_data)
{
  WbTray *self = user_data;
  self->registered_with_watcher = FALSE;
}

static void
on_bus_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
  WbTray *self = user_data;
  g_autoptr (GError) err = NULL;

  self->conn = connection;

  static const GDBusInterfaceVTable item_vtable = {
    item_method_call, item_get_property, NULL, { 0 }
  };
  static const GDBusInterfaceVTable menu_vtable = {
    menu_method_call, menu_get_property, NULL, { 0 }
  };

  self->item_reg_id = g_dbus_connection_register_object (connection, SNI_PATH, self->item_iface,
                                                          &item_vtable, self, NULL, &err);
  if (err != NULL) {
    log_error ("Tray: failed to export StatusNotifierItem: %s", err->message);
    g_clear_error (&err);
  }

  self->menu_reg_id = g_dbus_connection_register_object (connection, MENU_PATH, self->menu_iface,
                                                          &menu_vtable, self, NULL, &err);
  if (err != NULL) {
    log_error ("Tray: failed to export dbusmenu: %s", err->message);
    g_clear_error (&err);
  }

  register_with_watcher (self);
  self->watcher_watch_id = g_bus_watch_name_on_connection (connection, WATCHER_BUS,
                                                            G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                            on_watcher_appeared, on_watcher_vanished,
                                                            self, NULL);
}

static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer user_data)
{
  log_info ("Tray: could not own %s on the session bus; tray icon disabled", name);
}

/* The tray SVGs are compiled into the binary (GResource) and written out
   here at startup, then advertised through IconThemePath. That way the
   active AND inactive icons are always found by the tray host (waybar etc.)
   no matter how the app was installed or whether the hicolor icon cache is
   stale — nothing depends on an install script copying files around. */
static char *
export_bundled_icons (void)
{
  static const char *const names[] = { "dict-tray.svg", "dict-tray-inactive.svg" };
  const char *base = g_get_user_runtime_dir ();
  if (base == NULL || !g_file_test (base, G_FILE_TEST_IS_DIR))
    base = g_get_user_cache_dir ();

  char *dir = g_build_filename (base, "dict-tray", NULL);
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    log_warning ("Tray: cannot create icon dir %s", dir);
    g_free (dir);
    return NULL;
  }

  for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
    g_autoptr (GError) err = NULL;
    g_autofree char *res = g_strconcat (WB_RES_PATH, "/tray/", names[i], NULL);
    g_autofree char *dst = g_build_filename (dir, names[i], NULL);
    g_autoptr (GBytes) data = g_resources_lookup_data (res, G_RESOURCE_LOOKUP_FLAGS_NONE, &err);
    if (data == NULL) {
      log_warning ("Tray: bundled icon %s missing from resources: %s", names[i], err->message);
      continue;
    }
    gsize len = 0;
    const char *bytes = g_bytes_get_data (data, &len);
    if (!g_file_set_contents (dst, bytes, len, &err))
      log_warning ("Tray: could not write %s: %s", dst, err->message);
  }
  return dir;
}

WbTray *
wb_tray_new (const WbTrayCallbacks *callbacks)
{
  g_return_val_if_fail (callbacks != NULL, NULL);

  WbTray *self = g_new0 (WbTray, 1);
  self->cb = *callbacks;
  self->layout_revision = 1;
  self->icon_dir = export_bundled_icons ();

  g_autoptr (GError) err = NULL;
  self->node_info = g_dbus_node_info_new_for_xml (INTROSPECTION_XML, &err);
  if (self->node_info == NULL) {
    log_error ("Tray: bad introspection XML: %s", err->message);
    g_free (self);
    return NULL;
  }
  self->item_iface = g_dbus_node_info_lookup_interface (self->node_info, SNI_IFACE);
  self->menu_iface = g_dbus_node_info_lookup_interface (self->node_info, MENU_IFACE);

  self->service_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", getpid ());
  self->own_name_id = g_bus_own_name (G_BUS_TYPE_SESSION, self->service_name,
                                      G_BUS_NAME_OWNER_FLAGS_NONE,
                                      on_bus_acquired, NULL, on_name_lost, self, NULL);
  return self;
}

void
wb_tray_refresh (WbTray *tray)
{
  if (tray == NULL || tray->conn == NULL)
    return;
  tray->layout_revision++;
  g_dbus_connection_emit_signal (tray->conn, NULL, SNI_PATH, SNI_IFACE, "NewIcon", NULL, NULL);
  g_dbus_connection_emit_signal (tray->conn, NULL, SNI_PATH, SNI_IFACE, "NewToolTip", NULL, NULL);
  g_dbus_connection_emit_signal (tray->conn, NULL, MENU_PATH, MENU_IFACE, "LayoutUpdated",
                                 g_variant_new ("(ui)", tray->layout_revision, 0), NULL);
}

void
wb_tray_free (WbTray *tray)
{
  if (tray == NULL)
    return;
  if (tray->watcher_watch_id)
    g_bus_unwatch_name (tray->watcher_watch_id);
  if (tray->conn != NULL) {
    if (tray->item_reg_id)
      g_dbus_connection_unregister_object (tray->conn, tray->item_reg_id);
    if (tray->menu_reg_id)
      g_dbus_connection_unregister_object (tray->conn, tray->menu_reg_id);
  }
  if (tray->own_name_id)
    g_bus_unown_name (tray->own_name_id);
  g_clear_pointer (&tray->node_info, g_dbus_node_info_unref);
  g_free (tray->service_name);
  g_free (tray->icon_dir);
  g_free (tray);
}
