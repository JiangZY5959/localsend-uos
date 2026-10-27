/*
 * desktop_drop Linux 插件 —— UOS/国产环境兼容增强版（纯 C，自含声明，无需 GTK/GLib 开发头文件）
 *
 * 基于 MixinNetwork/flutter-plugins 的 desktop_drop 0.7.1（BSD-3-Clause）修改：
 *  1. 扩大接受的拖拽目标类型（部分国产/Qt/Wine 应用不提供标准 text/uri-list）
 *  2. 数据预处理：剥离 gnome-copied-files 的动作行；裸路径原样传给 Dart
 *  3. 可选调试日志：环境变量 DESKTOP_DROP_DEBUG=1 时输出到 stderr
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------- GLib / GObject 最小声明 ---------- */
typedef char gchar;
typedef unsigned char guchar;
typedef int gint;
typedef unsigned int guint;
typedef int gboolean;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef unsigned long gsize;

#define G_CALLBACK(f) ((gpointer) (f))

extern void g_printerr(const gchar *format, ...);
extern gchar *g_strdup(const gchar *str);
extern gchar *g_strndup(const gchar *str, gsize n);
extern void g_free(gpointer mem);
extern gpointer g_signal_connect_data(gpointer instance,
                                      const gchar *detailed_signal,
                                      gpointer c_handler, gpointer data,
                                      gpointer destroy_data, guint connect_flags);
#define g_signal_connect(instance, detailed_signal, c_handler, data) \
  g_signal_connect_data((instance), (detailed_signal), G_CALLBACK(c_handler), \
                        (data), NULL, (guint) 0)

/* ---------- GTK / GDK 最小声明 ---------- */
typedef struct _GtkWidget GtkWidget;
typedef struct _GtkSelectionData GtkSelectionData;
typedef struct _GdkDragContext GdkDragContext;
typedef struct _GdkAtom *GdkAtom;
typedef struct { gchar *target; guint flags; guint info; } GtkTargetEntry;

#define GTK_DEST_DEFAULT_MOTION (1 << 0)
#define GTK_DEST_DEFAULT_HIGHLIGHT (1 << 1)
#define GTK_DEST_DEFAULT_DROP (1 << 2)
#define GTK_DEST_DEFAULT_ALL 0x07
#define GDK_ACTION_COPY (1 << 0)
#define GDK_ACTION_MOVE (1 << 1)

extern void gtk_drag_dest_set(GtkWidget *widget, guint flags,
                              const GtkTargetEntry *targets, gint n_targets,
                              guint actions);
extern void gtk_drag_dest_add_uri_targets(GtkWidget *widget);
extern const guchar *gtk_selection_data_get_data(GtkSelectionData *data);
extern gint gtk_selection_data_get_length(GtkSelectionData *data);
extern GdkAtom gtk_selection_data_get_target(GtkSelectionData *data);
extern gchar *gdk_atom_name(GdkAtom atom);

/* ---------- Flutter Linux 嵌入层最小声明（签名取自 3.41.9 官方头文件） ---------- */
typedef struct _FlPluginRegistrar FlPluginRegistrar;
typedef struct _FlView FlView;
typedef struct _FlBinaryMessenger FlBinaryMessenger;
typedef struct _FlMethodChannel FlMethodChannel;
typedef struct _FlMethodCodec FlMethodCodec;
typedef struct _FlStandardMethodCodec FlStandardMethodCodec;
typedef struct _FlMethodCall FlMethodCall;
typedef struct _FlValue FlValue;
typedef void *GDestroyNotify_g;

typedef void (*FlMethodChannelMethodCallHandler)(FlMethodChannel *channel,
                                                 FlMethodCall *method_call,
                                                 gpointer user_data);

extern FlView *fl_plugin_registrar_get_view(FlPluginRegistrar *registrar);
extern FlBinaryMessenger *fl_plugin_registrar_get_messenger(
    FlPluginRegistrar *registrar);
extern FlMethodChannel *fl_method_channel_new(FlBinaryMessenger *messenger,
                                              const gchar *name,
                                              FlMethodCodec *codec);
extern void fl_method_channel_set_method_call_handler(
    FlMethodChannel *channel, FlMethodChannelMethodCallHandler handler,
    gpointer user_data, GDestroyNotify_g destroy_notify);
extern FlStandardMethodCodec *fl_standard_method_codec_new(void);
extern gboolean fl_method_call_respond_not_implemented(FlMethodCall *method_call,
                                                       gpointer *error);
extern FlValue *fl_value_new_list(void);
extern void fl_value_append(FlValue *value, FlValue *child);
extern FlValue *fl_value_new_string(const gchar *value);
extern FlValue *fl_value_new_float_list(const double *value, gsize length);
typedef struct _GCancellable GCancellable;
extern void fl_method_channel_invoke_method(FlMethodChannel *channel,
                                            const gchar *method, FlValue *args,
                                            GCancellable *cancellable,
                                            gpointer callback, gpointer user_data);

/* ---------- 插件逻辑 ---------- */
static gboolean on_focus_in_event(GtkWidget *widget, gpointer event,
                                  gpointer user_data);

static gboolean is_kde = 0;
static gboolean is_debug = 0;
static gboolean ignore_next = 0;

/* 把收到数据整理成 Dart 端能解析的行格式（每行一个 file:// URI 或本地路径） */
static gchar *normalize_data(const gchar *target, const guchar *raw, gint len) {
  gchar *text = g_strndup((const gchar *) raw, (gsize) len);

  /* gnome-copied-files：首行是 copy/cut/link 动作，去掉 */
  if (strstr(target, "gnome-copied-files") != NULL) {
    const gchar *nl = strchr(text, '\n');
    if (nl != NULL) {
      gchar *rest = g_strdup(nl + 1);
      g_free(text);
      text = rest;
    } else {
      g_free(text);
      return g_strdup("");
    }
  }
  /* x-kde4-urilist / uri-list / moz-url：保持原样，Dart 端逐行解析 */
  return text;
}

static void on_drag_data_received(GtkWidget *widget,
                                  GdkDragContext *drag_context, gint x, gint y,
                                  GtkSelectionData *sdata, guint info,
                                  guint time, gpointer user_data) {
  FlMethodChannel *channel = (FlMethodChannel *) user_data;
  const guchar *raw = gtk_selection_data_get_data(sdata);
  gint len = gtk_selection_data_get_length(sdata);
  GdkAtom atom = gtk_selection_data_get_target(sdata);
  gchar *target = gdk_atom_name(atom);

  if (raw == NULL || len <= 0) {
    if (is_debug)
      g_printerr("[desktop_drop] drop 空数据 target=%s\n",
                 target ? target : "?");
    g_free(target);
    return;
  }

  gchar *text = normalize_data(target ? target : "", raw, len);
  if (is_debug)
    g_printerr("[desktop_drop] target=%s data=%.200s\n",
               target ? target : "?", text);
  g_free(target);

  double point[2] = {(double) x, (double) y};
  FlValue *args = fl_value_new_list();
  fl_value_append(args, fl_value_new_string(text));
  fl_value_append(args, fl_value_new_float_list(point, 2));
  /* invoke_method 的 args 引用被容器持有，这里按原插件的做法不额外管理 */
  fl_method_channel_invoke_method(channel, "performOperation_linux", args,
                                  NULL, NULL, NULL);
  g_free(text);
}

static void on_drag_motion(GtkWidget *widget, GdkDragContext *drag_context,
                           gint x, gint y, guint time, gpointer user_data) {
  if (ignore_next) {
    ignore_next = 0;
    return;
  }
  FlMethodChannel *channel = (FlMethodChannel *) user_data;
  double point[2] = {(double) x, (double) y};
  FlValue *args = fl_value_new_float_list(point, 2);
  fl_method_channel_invoke_method(channel, "updated", args, NULL, NULL, NULL);
}

static void on_drag_leave(GtkWidget *widget, GdkDragContext *drag_context,
                          guint time, gpointer user_data) {
  FlMethodChannel *channel = (FlMethodChannel *) user_data;
  fl_method_channel_invoke_method(channel, "exited", NULL, NULL, NULL, NULL);
}

static void method_call_cb(FlMethodChannel *channel, FlMethodCall *method_call,
                           gpointer user_data) {
  /* Dart -> native 的调用全部不支持（与原插件一致） */
  fl_method_call_respond_not_implemented(method_call, NULL);
}

/* 插件注册入口（符号名由 Flutter 的 plugin registrant 约定） */
__attribute__((visibility("default")))
void desktop_drop_plugin_register_with_registrar(FlPluginRegistrar *registrar) {
  is_kde = 0;
  {
    const char *env = getenv("XDG_CURRENT_DESKTOP");
    if (env != NULL) {
      char buf[64];
      size_t i;
      for (i = 0; env[i] != '\0' && i < sizeof(buf) - 1; i++)
        buf[i] = (env[i] >= 'A' && env[i] <= 'Z') ? (char) (env[i] + 32) : env[i];
      buf[i] = '\0';
      if (strcmp(buf, "kde") == 0 || strcmp(buf, "plasma") == 0)
        is_kde = 1;
    }
  }
  is_debug = getenv("DESKTOP_DROP_DEBUG") != NULL;

  GtkWidget *view = (GtkWidget *) fl_plugin_registrar_get_view(registrar);

  /* 顺序即优先级：URI 列表优先，文本类兜底。
   * 部分国产应用（微信/WPS/桌面图标等）拖文件时只提供 text/plain 或私有类型，
   * 原版只认 STRING + uri-list，导致“拖进去没反应”。 */
  static GtkTargetEntry entries[] = {
      {(gchar *) "text/uri-list", 4, 0},                      /* OTHER_APP */
      {(gchar *) "application/x-kde4-urilist", 4, 1},
      {(gchar *) "x-special/gnome-copied-files", 4, 2},
      {(gchar *) "application/x-gnome-copied-files", 4, 3},
      {(gchar *) "text/x-moz-url", 4, 4},
      {(gchar *) "text/plain", 4, 5},
      {(gchar *) "UTF8_STRING", 4, 6},
      {(gchar *) "STRING", 4, 7},
      {(gchar *) "TEXT", 4, 8},
      {(gchar *) "COMPOUND_TEXT", 4, 9},
  };
  gtk_drag_dest_set(view, GTK_DEST_DEFAULT_ALL, entries,
                    (gint) (sizeof(entries) / sizeof(entries[0])),
                    GDK_ACTION_COPY | GDK_ACTION_MOVE);
  gtk_drag_dest_add_uri_targets(view);

  FlStandardMethodCodec *codec = fl_standard_method_codec_new();
  FlMethodChannel *channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar), "desktop_drop",
      (FlMethodCodec *) codec);

  g_signal_connect(view, "drag-motion", on_drag_motion, channel);
  g_signal_connect(view, "drag-data-received", on_drag_data_received, channel);
  g_signal_connect(view, "drag-leave", on_drag_leave, channel);
  g_signal_connect(view, "focus-in-event", on_focus_in_event, NULL);
}

/* KDE 焦点恢复会多触发一次 drag-motion（上游插件的规避逻辑） */
static gboolean on_focus_in_event(GtkWidget *widget, gpointer event,
                                  gpointer user_data) {
  if (is_kde) {
    ignore_next = 1;
  }
  return 0;
}
