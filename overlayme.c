#if 0
set -e

SRC="${1:-overlayme.c}"
VENV=".venv"

echo "Cooking! Hold tight...."

python3 -m venv "$VENV"
source "$VENV/bin/activate"

python -m pip install -q --upgrade pip zenity cmake static-ffmpeg

#Force static - ffmpeg to fetch its binaries now.
static_ffmpeg -version >/dev/null 2>&1
static_ffprobe -version >/dev/null 2>&1

FFMPEG_BIN="$VIRTUAL_ENV/bin/static_ffmpeg"
FFPROBE_BIN="$VIRTUAL_ENV/bin/static_ffprobe"

mkdir -p "$VENV/src"

[ -d "$VENV/src/raylib" ] || \
    git clone -q --depth 1 --branch 6.0 \
    https://github.com/raysan5/raylib.git \
    "$VENV/src/raylib"

rm -rf "$VENV/src/raylib/build"

cmake \
    -S "$VENV/src/raylib" \
    -B "$VENV/src/raylib/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$VIRTUAL_ENV" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF

cmake --build "$VENV/src/raylib/build" \
    --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || \
                 sysctl -n hw.ncpu 2>/dev/null || echo 4)"

cmake --install "$VENV/src/raylib/build"

RAYLIB="$(find "$VIRTUAL_ENV" -name libraylib.a | head -1)"

case "$(uname -s)" in
Darwin)
    LIBS=(
        -framework OpenGL
        -framework OpenAL
        -framework IOKit
        -framework CoreVideo
        -framework Cocoa
    )
    ;;
Linux)
    LIBS=(-lGL -lm -lpthread -ldl -lrt -lX11)
    ;;
*)
    echo "Unsupported OS"
    exit 1
    ;;
esac

clang \
    -O3 \
    -g \
    -std=c11 \
    -I"$VIRTUAL_ENV/include" \
    -DFFMPEG_BIN="\"$FFMPEG_BIN\"" \
    -DFFPROBE_BIN="\"$FFPROBE_BIN\"" \
    "$SRC" \
    "$RAYLIB" \
    -o overlayme \
    "${LIBS[@]}"

echo "Built: ./overlayme"

./overlayme

exit
#endif

#define _POSIX_C_SOURCE 200809L

#include "raylib.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FFMPEG_BIN
#define FFMPEG_BIN "ffmpeg"
#endif

#ifndef FFPROBE_BIN
#define FFPROBE_BIN "ffprobe"
#endif

#define PATH_CAP 8192

typedef struct {
  Texture2D texture;
  int width;
  int height;
  bool loaded;
  char path[PATH_CAP];
} Background;

/*
 * GIFs are now streamed by FFmpeg.
 *
 * Only ONE decoded RGBA frame is kept in RAM.
 *
 * Old implementation:
 *
 *     width * height * 4 * number_of_frames
 *
 * New implementation:
 *
 *     width * height * 4
 */
typedef struct {
  Texture2D texture;

  unsigned char *frameData;
  size_t frameBytes;

  int width;
  int height;
  uint64_t frame;

  float accumulator;
  float previewFps;

  bool playing;
  bool loaded;

  FILE *ffmpegPipe;
  pid_t ffmpegPid;

  char path[PATH_CAP];
} AnimatedGif;

typedef enum {
  DRAG_NONE = 0,
  DRAG_MOVE,
  DRAG_TL,
  DRAG_TR,
  DRAG_BL,
  DRAG_BR
} DragMode;

/* -------------------------------------------------------------------------- */
/* UI helpers                                                                 */
/* -------------------------------------------------------------------------- */

static bool button_ex(Rectangle r, const char *text, bool enabled,
                      bool primary) {
  Vector2 m = GetMousePosition();
  bool hover = enabled && CheckCollisionPointRec(m, r);

  Color fill;
  Color border;
  Color fg;

  if (!enabled) {
    fill = (Color){36, 38, 42, 255};
    border = (Color){49, 52, 58, 255};
    fg = (Color){105, 108, 114, 255};
  } else if (primary) {
    fill = hover ? (Color){67, 139, 235, 255} : (Color){55, 121, 214, 255};

    border = fill;
    fg = RAYWHITE;
  } else {
    fill = hover ? (Color){52, 55, 61, 255} : (Color){42, 45, 50, 255};

    border = hover ? (Color){79, 84, 92, 255} : (Color){61, 65, 72, 255};

    fg = (Color){235, 237, 240, 255};
  }

  DrawRectangleRounded(r, 0.18f, 8, fill);
  DrawRectangleRoundedLinesEx(r, 0.18f, 8, 1.0f, border);

  int fs = 16;
  int tw = MeasureText(text, fs);

  DrawText(text, (int)(r.x + (r.width - tw) * 0.5f),
           (int)(r.y + (r.height - fs) * 0.5f - 1), fs, fg);

  return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static bool button(Rectangle r, const char *text, bool enabled) {
  return button_ex(r, text, enabled, false);
}

static bool primary_button(Rectangle r, const char *text, bool enabled) {
  return button_ex(r, text, enabled, true);
}

static void divider(float x, float y, float width) {
  DrawRectangle((int)x, (int)y, (int)width, 1, (Color){54, 57, 63, 255});
}

static void label_text(const char *text, float x, float y) {
  DrawText(text, (int)x, (int)y, 14, (Color){145, 149, 157, 255});
}

static void value_box(Rectangle r, const char *label, const char *value) {
  DrawRectangleRounded(r, 0.14f, 6, (Color){27, 29, 33, 255});

  DrawRectangleRoundedLinesEx(r, 0.14f, 6, 1.0f, (Color){50, 53, 59, 255});

  DrawText(label, (int)r.x + 11, (int)r.y + 8, 12, (Color){132, 136, 144, 255});

  DrawText(value, (int)r.x + 11, (int)r.y + 25, 16,
           (Color){235, 237, 240, 255});
}

static float slider(Rectangle r, float value) {
  static bool active = false;

  Vector2 m = GetMousePosition();

  bool hover = CheckCollisionPointRec(
      m, (Rectangle){r.x, r.y - 8, r.width, r.height + 16});

  if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    active = true;

  if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    active = false;

  if (active && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    value = (m.x - r.x) / r.width;

  value = fmaxf(0.0f, fminf(value, 1.0f));

  float cy = r.y + r.height * 0.5f;

  DrawRectangleRounded((Rectangle){r.x, cy - 2, r.width, 4}, 1.0f, 4,
                       (Color){55, 59, 65, 255});

  DrawRectangleRounded((Rectangle){r.x, cy - 2, r.width * value, 4}, 1.0f, 4,
                       (Color){55, 121, 214, 255});

  DrawCircleV((Vector2){r.x + r.width * value, cy}, 7.0f,
              active || hover ? RAYWHITE : (Color){218, 221, 226, 255});

  return value;
}

static void draw_filename(const char *path, float x, float y, float maxWidth) {
  const char *name = path && path[0] ? GetFileName(path) : "Not selected";

  char shown[128];

  snprintf(shown, sizeof(shown), "%s", name);

  while (shown[0] && MeasureText(shown, 14) > (int)maxWidth) {

    size_t n = strlen(shown);

    if (n <= 4)
      break;

    shown[n - 1] = '\0';

    if (n > 4) {
      shown[n - 4] = '.';
      shown[n - 3] = '.';
      shown[n - 2] = '.';
      shown[n - 1] = '\0';
    }
  }

  DrawText(shown, (int)x, (int)y, 14,
           path && path[0] ? (Color){191, 195, 202, 255}
                           : (Color){110, 114, 121, 255});
}

/* -------------------------------------------------------------------------- */
/* File helpers                                                               */
/* -------------------------------------------------------------------------- */

static bool has_ext_ci(const char *path, const char *ext) {
  size_t a = strlen(path);
  size_t b = strlen(ext);

  if (a < b)
    return false;

  path += a - b;

  for (size_t i = 0; i < b; ++i) {
    if (tolower((unsigned char)path[i]) != tolower((unsigned char)ext[i]))
      return false;
  }

  return true;
}

static bool is_gif(const char *path) { return has_ext_ci(path, ".gif"); }

static bool is_background_image(const char *path) {
  return has_ext_ci(path, ".png") || has_ext_ci(path, ".jpg") ||
         has_ext_ci(path, ".jpeg") || has_ext_ci(path, ".bmp") ||
         has_ext_ci(path, ".tga");
}

/* -------------------------------------------------------------------------- */
/* Background                                                                 */
/* -------------------------------------------------------------------------- */

static void unload_background(Background *bg) {
  if (bg->loaded)
    UnloadTexture(bg->texture);

  memset(bg, 0, sizeof(*bg));
}

static bool load_background(Background *bg, const char *path) {
  Image img = LoadImage(path);

  if (!img.data)
    return false;

  Texture2D texture = LoadTextureFromImage(img);

  if (texture.id == 0) {
    UnloadImage(img);
    return false;
  }

  unload_background(bg);

  bg->texture = texture;
  bg->width = img.width;
  bg->height = img.height;
  bg->loaded = true;

  snprintf(bg->path, sizeof(bg->path), "%s", path);

  UnloadImage(img);

  return true;
}

/* -------------------------------------------------------------------------- */
/* FFmpeg GIF streaming                                                       */
/* -------------------------------------------------------------------------- */

static float parse_fraction(const char *s) {
  int numerator = 0;
  int denominator = 0;

  if (sscanf(s, "%d/%d", &numerator, &denominator) == 2) {

    if (denominator != 0)
      return (float)numerator / (float)denominator;
  }

  float v = 0.0f;

  if (sscanf(s, "%f", &v) == 1)
    return v;

  return 0.0f;
}

/*
 * Probe:
 *
 * width,height,average_framerate
 *
 * Example:
 *
 * 1920,1080,20/1
 */
static bool probe_gif(const char *path, int *width, int *height, float *fps) {
  int pipefd[2];

  if (pipe(pipefd) != 0)
    return false;

  pid_t pid = fork();

  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (pid == 0) {

    dup2(pipefd[1], STDOUT_FILENO);

    close(pipefd[0]);
    close(pipefd[1]);

    execl(FFPROBE_BIN, "ffprobe",

          "-v", "error",

          "-select_streams", "v:0",

          "-show_entries", "stream=width,height,avg_frame_rate",

          "-of", "csv=p=0",

          path,

          (char *)NULL);

    _exit(127);
  }

  close(pipefd[1]);

  char output[512];
  size_t used = 0;

  while (used + 1 < sizeof(output)) {

    ssize_t n = read(pipefd[0], output + used, sizeof(output) - used - 1);

    if (n > 0) {
      used += (size_t)n;
      continue;
    }

    if (n < 0 && errno == EINTR)
      continue;

    break;
  }

  output[used] = '\0';

  close(pipefd[0]);

  int status = 0;

  if (waitpid(pid, &status, 0) < 0)
    return false;

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return false;

  int w = 0;
  int h = 0;
  char rate[64] = {0};

  int parsed = sscanf(output, "%d,%d,%63[^\r\n]", &w, &h, rate);

  if (parsed < 2)
    return false;

  if (w <= 0 || h <= 0)
    return false;

  float detectedFps = 20.0f;

  if (parsed >= 3) {
    float v = parse_fraction(rate);

    if (isfinite(v) && v >= 1.0f && v <= 240.0f) {

      detectedFps = v;
    }
  }

  *width = w;
  *height = h;
  *fps = detectedFps;

  return true;
}

static bool read_exact(FILE *fp, void *destination, size_t bytes) {
  unsigned char *p = (unsigned char *)destination;

  size_t done = 0;

  while (done < bytes) {

    size_t n = fread(p + done, 1, bytes - done, fp);

    if (n > 0) {
      done += n;
      continue;
    }

    if (feof(fp))
      return false;

    if (ferror(fp))
      return false;
  }

  return true;
}

static void stop_gif_decoder(AnimatedGif *gif) {
  if (gif->ffmpegPid > 0) {

    /*
     * Stop producer first. This prevents us waiting for a
     * permanently blocked FFmpeg writer.
     */
    kill(gif->ffmpegPid, SIGTERM);
  }

  if (gif->ffmpegPipe) {

    fclose(gif->ffmpegPipe);

    gif->ffmpegPipe = NULL;
  }

  if (gif->ffmpegPid > 0) {

    int status = 0;

    while (waitpid(gif->ffmpegPid, &status, 0) < 0) {

      if (errno != EINTR)
        break;
    }
  }

  gif->ffmpegPid = -1;
}

static bool start_gif_decoder(AnimatedGif *gif) {
  int pipefd[2];

  if (pipe(pipefd) != 0)
    return false;

  pid_t pid = fork();

  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (pid == 0) {

    dup2(pipefd[1], STDOUT_FILENO);

    close(pipefd[0]);
    close(pipefd[1]);

    /*
     * -stream_loop -1:
     *     repeat the input forever
     *
     * Output:
     *     raw RGBA frames on stdout
     */
    execl(FFMPEG_BIN, "ffmpeg",

          "-hide_banner", "-loglevel", "error",

          "-nostdin",

          "-stream_loop", "-1",

          "-i", gif->path,

          "-an", "-sn", "-dn",

          "-fps_mode", "passthrough",

          "-f", "rawvideo",

          "-pix_fmt", "rgba",

          "pipe:1",

          (char *)NULL);

    _exit(127);
  }

  close(pipefd[1]);

  FILE *fp = fdopen(pipefd[0], "rb");

  if (!fp) {

    close(pipefd[0]);

    kill(pid, SIGTERM);

    waitpid(pid, NULL, 0);

    return false;
  }

  gif->ffmpegPipe = fp;
  gif->ffmpegPid = pid;

  return true;
}

static bool restart_gif_decoder(AnimatedGif *gif) {
  stop_gif_decoder(gif);

  return start_gif_decoder(gif);
}

static void unload_gif(AnimatedGif *gif) {
  stop_gif_decoder(gif);

  if (gif->texture.id != 0)
    UnloadTexture(gif->texture);

  free(gif->frameData);

  memset(gif, 0, sizeof(*gif));

  gif->ffmpegPid = -1;
}

static bool load_gif(AnimatedGif *gif, const char *path) {
  AnimatedGif tmp = {0};

  tmp.ffmpegPid = -1;

  snprintf(tmp.path, sizeof(tmp.path), "%s", path);

  float detectedFps = 20.0f;

  if (!probe_gif(path, &tmp.width, &tmp.height, &detectedFps)) {

    fprintf(stderr, "Could not probe GIF: %s\n", path);

    return false;
  }

  if (tmp.width <= 0 || tmp.height <= 0)
    return false;

  /*
   * Protect the multiplication itself.
   */
  if ((size_t)tmp.width > SIZE_MAX / 4u / (size_t)tmp.height) {

    fprintf(stderr, "GIF frame dimensions are too large\n");

    return false;
  }

  tmp.frameBytes = (size_t)tmp.width * (size_t)tmp.height * 4u;

  tmp.frameData = malloc(tmp.frameBytes);

  if (!tmp.frameData) {

    fprintf(stderr, "Could not allocate %zu bytes for GIF frame\n",
            tmp.frameBytes);

    return false;
  }

  tmp.previewFps = detectedFps;

  /*
   * Keep ridiculous/incorrect metadata from killing
   * the preview timing.
   */
  if (tmp.previewFps < 1.0f)
    tmp.previewFps = 20.0f;

  if (tmp.previewFps > 120.0f)
    tmp.previewFps = 120.0f;

  fprintf(stderr, "GIF: %dx%d, %.2f fps, frame buffer %.2f MiB\n", tmp.width,
          tmp.height, tmp.previewFps,
          (double)tmp.frameBytes / (1024.0 * 1024.0));

  if (!start_gif_decoder(&tmp)) {

    fprintf(stderr, "Could not start FFmpeg GIF decoder\n");

    unload_gif(&tmp);

    return false;
  }

  /*
   * Decode the first frame before creating the texture.
   */
  if (!read_exact(tmp.ffmpegPipe, tmp.frameData, tmp.frameBytes)) {

    fprintf(stderr, "Could not decode first GIF frame\n");

    unload_gif(&tmp);

    return false;
  }

  Image firstFrame = {.data = tmp.frameData,
                      .width = tmp.width,
                      .height = tmp.height,
                      .mipmaps = 1,
                      .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};

  tmp.texture = LoadTextureFromImage(firstFrame);

  if (tmp.texture.id == 0) {

    fprintf(stderr, "Could not create GIF texture\n");

    unload_gif(&tmp);

    return false;
  }

  tmp.frame = 0;
  tmp.accumulator = 0.0f;
  tmp.playing = true;
  tmp.loaded = true;

  /*
   * Only destroy the existing GIF AFTER the new one
   * has loaded successfully.
   */
  unload_gif(gif);

  *gif = tmp;

  return true;
}

static void update_gif(AnimatedGif *gif) {
  if (!gif->loaded || !gif->playing || !gif->ffmpegPipe)
    return;

  gif->accumulator += GetFrameTime();

  float dt = 1.0f / fmaxf(gif->previewFps, 1.0f);

  /*
   * Avoid spending forever catching up after the
   * application stalls or gets dragged between screens.
   */
  if (gif->accumulator > 0.25f)
    gif->accumulator = 0.25f;

  while (gif->accumulator >= dt) {

    gif->accumulator -= dt;

    if (!read_exact(gif->ffmpegPipe, gif->frameData, gif->frameBytes)) {

      /*
       * FFmpeg unexpectedly exited. Restart the
       * decoder and continue from frame zero.
       */
      fprintf(stderr, "GIF decoder ended; restarting\n");

      if (!restart_gif_decoder(gif)) {
        gif->playing = false;
        return;
      }

      if (!read_exact(gif->ffmpegPipe, gif->frameData, gif->frameBytes)) {

        gif->playing = false;
        return;
      }

      gif->frame = 0;
    } else {
      ++gif->frame;
    }

    UpdateTexture(gif->texture, gif->frameData);
  }
}

/* -------------------------------------------------------------------------- */
/* Zenity                                                                     */
/* -------------------------------------------------------------------------- */

static bool pick_with_zenity(char *out, size_t outSize, const char *title,
                             const char *filter, bool save) {
  char cmd[1024];

  if (save) {

    snprintf(cmd, sizeof(cmd),

             "zenity --file-selection "
             "--save "
             "--confirm-overwrite "
             "--title='%s' "
             "--filename='overlay.png' "
             "--file-filter='%s' "
             "2>/dev/null",

             title, filter);
  } else {

    snprintf(cmd, sizeof(cmd),

             "zenity --file-selection "
             "--title='%s' "
             "--file-filter='%s' "
             "2>/dev/null",

             title, filter);
  }

  FILE *fp = popen(cmd, "r");

  if (!fp)
    return false;

  char tmp[PATH_CAP];

  if (!fgets(tmp, sizeof(tmp), fp)) {

    pclose(fp);
    return false;
  }

  int rc = pclose(fp);

  if (rc != 0)
    return false;

  tmp[strcspn(tmp, "\r\n")] = '\0';

  if (!tmp[0])
    return false;

  snprintf(out, outSize, "%s", tmp);

  return true;
}

/* -------------------------------------------------------------------------- */
/* Export                                                                     */
/* -------------------------------------------------------------------------- */

static int export_apng(const char *bgPath, const char *gifPath,
                       Rectangle overlay, float realism, const char *outPath) {
  int x = (int)lroundf(overlay.x);
  int y = (int)lroundf(overlay.y);

  int w = (int)lroundf(overlay.width);
  int h = (int)lroundf(overlay.height);

  float t = fmaxf(0.0f, fminf(realism, 1.0f));

  float brightness = -0.035f * t;

  float contrast = 1.0f + 0.05f * t;

  float saturation = 1.0f - 0.08f * t;

  float opacity = 1.0f - 0.05f * t;

  char filter[1024];

  snprintf(filter, sizeof(filter),

           "[1:v]"
           "scale=%d:%d:flags=lanczos,"
           "eq=brightness=%.4f:"
           "contrast=%.4f:"
           "saturation=%.4f,"
           "format=rgba,"
           "colorchannelmixer=aa=%.4f"
           "[ov];"

           "[0:v]"
           "format=rgba"
           "[bg];"

           "[bg][ov]"
           "overlay=x=%d:y=%d:"
           "shortest=1:"
           "format=auto"
           "[out]",

           w, h, brightness, contrast, saturation, opacity, x, y);

  pid_t pid = fork();

  if (pid < 0)
    return -1;

  if (pid == 0) {

    execl(FFMPEG_BIN, "ffmpeg",

          "-y",

          "-loop", "1",

          "-i", bgPath,

          "-ignore_loop", "1",

          "-i", gifPath,

          "-filter_complex", filter,

          "-map", "[out]",

          "-f", "apng",

          "-plays", "0",

          outPath,

          (char *)NULL);

    _exit(127);
  }

  int status = 0;

  if (waitpid(pid, &status, 0) < 0)
    return -1;

  if (WIFEXITED(status))
    return WEXITSTATUS(status);

  return -1;
}

/* -------------------------------------------------------------------------- */
/* Camera / overlay helpers                                                   */
/* -------------------------------------------------------------------------- */

static float fit_scale(float srcW, float srcH, Rectangle area) {
  return fminf(area.width / srcW, area.height / srcH);
}

static Camera2D make_camera(const Background *bg, Rectangle area, float zoom,
                            Vector2 center) {
  Camera2D camera = {0};

  camera.offset =
      (Vector2){area.x + area.width * 0.5f, area.y + area.height * 0.5f};

  camera.target = center;
  camera.rotation = 0.0f;

  camera.zoom = fit_scale((float)bg->width, (float)bg->height, area) * zoom;

  return camera;
}

static void reset_view(const Background *bg, float *zoom, Vector2 *center) {
  if (!bg->loaded)
    return;

  *zoom = 1.0f;

  *center = (Vector2){bg->width * 0.5f, bg->height * 0.5f};
}

static Rectangle image_to_screen(Rectangle r, Camera2D camera) {
  Vector2 tl = GetWorldToScreen2D((Vector2){r.x, r.y}, camera);

  Vector2 br =
      GetWorldToScreen2D((Vector2){r.x + r.width, r.y + r.height}, camera);

  return (Rectangle){tl.x, tl.y, br.x - tl.x, br.y - tl.y};
}

static Rectangle clamp_overlay(Rectangle r, int imageW, int imageH) {
  const float minSize = 16.0f;

  if (r.width < minSize)
    r.width = minSize;

  if (r.height < minSize)
    r.height = minSize;

  if (r.width > imageW)
    r.width = (float)imageW;

  if (r.height > imageH)
    r.height = (float)imageH;

  if (r.x < 0)
    r.x = 0;

  if (r.y < 0)
    r.y = 0;

  if (r.x + r.width > imageW)
    r.x = imageW - r.width;

  if (r.y + r.height > imageH)
    r.y = imageH - r.height;

  return r;
}

static void set_default_overlay(Rectangle *overlay, const Background *bg,
                                const AnimatedGif *gif) {
  if (!bg->loaded || !gif->loaded)
    return;

  float targetW = bg->width * 0.38f;

  float targetH = targetW * ((float)gif->height / (float)gif->width);

  if (targetH > bg->height * 0.55f) {

    targetH = bg->height * 0.55f;

    targetW = targetH * ((float)gif->width / (float)gif->height);
  }

  *overlay = (Rectangle){(bg->width - targetW) * 0.5f,
                         (bg->height - targetH) * 0.5f, targetW, targetH};
}

static Rectangle handle_rect(Vector2 p, float size) {
  return (Rectangle){p.x - size * 0.5f, p.y - size * 0.5f, size, size};
}

static void draw_handle(Vector2 p, float size) {
  Rectangle r = handle_rect(p, size);

  DrawRectangleRounded(r, 0.22f, 5, RAYWHITE);

  DrawRectangleRoundedLinesEx(r, 0.22f, 5, 1.0f, (Color){30, 34, 40, 255});
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);

  InitWindow(1450, 900, "OverlayMe");

  SetTargetFPS(60);

  Background bg = {0};

  AnimatedGif gif = {0};
  gif.ffmpegPid = -1;

  Rectangle overlay = {0};

  char status[512] =
      "Drop a background image and a GIF here, or use the buttons.";

  if (argc >= 2 && load_background(&bg, argv[1])) {

    snprintf(status, sizeof(status), "Loaded background: %s",
             GetFileName(bg.path));
  }

  if (argc >= 3 && load_gif(&gif, argv[2])) {

    snprintf(status, sizeof(status), "Loaded GIF: %s", GetFileName(gif.path));
  }

  if (bg.loaded && gif.loaded) {

    set_default_overlay(&overlay, &bg, &gif);
  }

  DragMode dragMode = DRAG_NONE;

  Rectangle dragStart = {0};

  Vector2 dragStartImage = {0};

  float viewZoom = 1.0f;
  float realism = 0.0f;

  Vector2 viewCenter = {0};

  bool panning = false;

  Vector2 panStartMouse = {0};
  Vector2 panStartCenter = {0};

  if (bg.loaded) {

    reset_view(&bg, &viewZoom, &viewCenter);
  }

  while (!WindowShouldClose()) {

    update_gif(&gif);

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    const float sidebarW = 330.0f;
    const float margin = 18.0f;

    Rectangle canvas = {margin, margin,

                        fmaxf(300.0f, sw - sidebarW - margin * 3.0f),

                        sh - margin * 2.0f};

    Rectangle sidebar = {canvas.x + canvas.width + margin,

                         margin,

                         sidebarW,

                         sh - margin * 2.0f};

    Camera2D camera = {0};

    if (bg.loaded) {

      camera = make_camera(&bg, canvas, viewZoom, viewCenter);
    }

    /* ------------------------------------------------------------------ */
    /* Drag/drop                                                          */
    /* ------------------------------------------------------------------ */

    if (IsFileDropped()) {

      FilePathList files = LoadDroppedFiles();

      bool changed = false;

      for (unsigned int i = 0; i < files.count; ++i) {

        const char *p = files.paths[i];

        if (is_gif(p)) {

          if (load_gif(&gif, p)) {

            snprintf(status, sizeof(status), "Loaded GIF: %s", GetFileName(p));

            changed = true;
          } else {

            snprintf(status, sizeof(status), "Could not load GIF: %s", p);
          }
        } else if (is_background_image(p)) {

          if (load_background(&bg, p)) {

            snprintf(status, sizeof(status), "Loaded background: %s",
                     GetFileName(p));

            reset_view(&bg, &viewZoom, &viewCenter);

            camera = make_camera(&bg, canvas, viewZoom, viewCenter);

            changed = true;
          } else {

            snprintf(status, sizeof(status), "Could not load image: %s", p);
          }
        }
      }

      UnloadDroppedFiles(files);

      if (changed && bg.loaded && gif.loaded) {

        set_default_overlay(&overlay, &bg, &gif);
      }
    }

    /* ------------------------------------------------------------------ */
    /* Keyboard overlay movement                                          */
    /* ------------------------------------------------------------------ */

    if (bg.loaded && gif.loaded && dragMode == DRAG_NONE && !panning) {

      float step = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)
                       ? 10.0f
                       : 1.0f;

      bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);

      bool changed = false;

      if (!alt) {

        if (IsKeyPressed(KEY_LEFT)) {
          overlay.x -= step;
          changed = true;
        }

        if (IsKeyPressed(KEY_RIGHT)) {
          overlay.x += step;
          changed = true;
        }

        if (IsKeyPressed(KEY_UP)) {
          overlay.y -= step;
          changed = true;
        }

        if (IsKeyPressed(KEY_DOWN)) {
          overlay.y += step;
          changed = true;
        }
      } else {

        if (IsKeyPressed(KEY_LEFT)) {
          overlay.width -= step;
          changed = true;
        }

        if (IsKeyPressed(KEY_RIGHT)) {
          overlay.width += step;
          changed = true;
        }

        if (IsKeyPressed(KEY_UP)) {
          overlay.height -= step;
          changed = true;
        }

        if (IsKeyPressed(KEY_DOWN)) {
          overlay.height += step;
          changed = true;
        }
      }

      if (changed) {

        overlay = clamp_overlay(overlay, bg.width, bg.height);
      }

      if (IsKeyPressed(KEY_SPACE))
        gif.playing = !gif.playing;
    }

    Rectangle overlayScreen = {0};

    /* ------------------------------------------------------------------ */
    /* Camera                                                             */
    /* ------------------------------------------------------------------ */

    if (bg.loaded) {

      Vector2 mouse = GetMousePosition();

      bool overCanvas = CheckCollisionPointRec(mouse, canvas);

      float wheel = GetMouseWheelMove();

      if (overCanvas && wheel != 0.0f) {

        Vector2 before = GetScreenToWorld2D(mouse, camera);

        float newZoom = viewZoom * powf(1.20f, wheel);

        newZoom = fmaxf(0.10f, fminf(newZoom, 64.0f));

        if (fabsf(newZoom - viewZoom) > 0.0001f) {

          viewZoom = newZoom;

          camera = make_camera(&bg, canvas, viewZoom, viewCenter);

          Vector2 after = GetScreenToWorld2D(mouse, camera);

          viewCenter.x += before.x - after.x;

          viewCenter.y += before.y - after.y;

          camera = make_camera(&bg, canvas, viewZoom, viewCenter);
        }
      }

      bool panPressed = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
                        IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);

      if (overCanvas && panPressed) {

        panning = true;

        panStartMouse = mouse;

        panStartCenter = viewCenter;

        dragMode = DRAG_NONE;
      }

      if (panning) {

        bool panHeld = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
                       IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

        if (panHeld) {

          Vector2 delta = {mouse.x - panStartMouse.x,

                           mouse.y - panStartMouse.y};

          viewCenter.x = panStartCenter.x - delta.x / camera.zoom;

          viewCenter.y = panStartCenter.y - delta.y / camera.zoom;

          camera = make_camera(&bg, canvas, viewZoom, viewCenter);
        } else {

          panning = false;
        }
      }

      bool zoomIn = IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD) ||
                    IsKeyPressed(KEY_X);

      bool zoomOut = IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT) ||
                     IsKeyPressed(KEY_Z);

      if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_F)) {

        reset_view(&bg, &viewZoom, &viewCenter);

        camera = make_camera(&bg, canvas, viewZoom, viewCenter);
      } else if (zoomIn || zoomOut) {

        Vector2 anchor = overCanvas ? mouse : camera.offset;

        Vector2 before = GetScreenToWorld2D(anchor, camera);

        if (zoomIn) {

          viewZoom = fminf(viewZoom * 1.20f, 64.0f);
        }

        if (zoomOut) {

          viewZoom = fmaxf(viewZoom / 1.20f, 0.10f);
        }

        camera = make_camera(&bg, canvas, viewZoom, viewCenter);

        Vector2 after = GetScreenToWorld2D(anchor, camera);

        viewCenter.x += before.x - after.x;

        viewCenter.y += before.y - after.y;

        camera = make_camera(&bg, canvas, viewZoom, viewCenter);
      }
    }

    /* ------------------------------------------------------------------ */
    /* Overlay drag/resize                                                */
    /* ------------------------------------------------------------------ */

    if (bg.loaded && gif.loaded) {

      overlayScreen = image_to_screen(overlay, camera);

      float handleSize = 14.0f;

      Vector2 tl = {overlayScreen.x, overlayScreen.y};

      Vector2 tr = {overlayScreen.x + overlayScreen.width,

                    overlayScreen.y};

      Vector2 bl = {overlayScreen.x,

                    overlayScreen.y + overlayScreen.height};

      Vector2 br = {overlayScreen.x + overlayScreen.width,

                    overlayScreen.y + overlayScreen.height};

      Vector2 mouse = GetMousePosition();

      if (!panning && CheckCollisionPointRec(mouse, canvas) &&
          IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {

        if (CheckCollisionPointRec(mouse, handle_rect(tl, handleSize + 6))) {

          dragMode = DRAG_TL;
        } else if (CheckCollisionPointRec(mouse,
                                          handle_rect(tr, handleSize + 6))) {

          dragMode = DRAG_TR;
        } else if (CheckCollisionPointRec(mouse,
                                          handle_rect(bl, handleSize + 6))) {

          dragMode = DRAG_BL;
        } else if (CheckCollisionPointRec(mouse,
                                          handle_rect(br, handleSize + 6))) {

          dragMode = DRAG_BR;
        } else if (CheckCollisionPointRec(mouse, overlayScreen)) {

          dragMode = DRAG_MOVE;
        }

        if (dragMode != DRAG_NONE) {

          dragStart = overlay;

          dragStartImage = GetScreenToWorld2D(mouse, camera);
        }
      }

      if (dragMode != DRAG_NONE && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {

        Vector2 now = GetScreenToWorld2D(mouse, camera);

        float dx = now.x - dragStartImage.x;

        float dy = now.y - dragStartImage.y;

        Rectangle r = dragStart;

        switch (dragMode) {

        case DRAG_MOVE:
          r.x += dx;
          r.y += dy;
          break;

        case DRAG_TL:
          r.x += dx;
          r.y += dy;
          r.width -= dx;
          r.height -= dy;
          break;

        case DRAG_TR:
          r.y += dy;
          r.width += dx;
          r.height -= dy;
          break;

        case DRAG_BL:
          r.x += dx;
          r.width -= dx;
          r.height += dy;
          break;

        case DRAG_BR:
          r.width += dx;
          r.height += dy;
          break;

        default:
          break;
        }

        if (r.width >= 16.0f && r.height >= 16.0f) {

          overlay = clamp_overlay(r, bg.width, bg.height);
        }
      }

      if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {

        dragMode = DRAG_NONE;
      }
    }

    /* ------------------------------------------------------------------ */
    /* Draw                                                               */
    /* ------------------------------------------------------------------ */

    BeginDrawing();

    ClearBackground((Color){24, 27, 32, 255});

    DrawRectangleRounded(canvas, 0.018f, 8, (Color){16, 18, 22, 255});

    DrawRectangleRoundedLinesEx(canvas, 0.018f, 8, 1.0f,
                                (Color){58, 63, 72, 255});

    if (bg.loaded) {

      BeginScissorMode((int)canvas.x, (int)canvas.y, (int)canvas.width,
                       (int)canvas.height);

      BeginMode2D(camera);

      DrawTexture(bg.texture, 0, 0, WHITE);

      if (gif.loaded) {

        float t = fmaxf(0.0f, fminf(realism, 1.0f));

        unsigned char shade = (unsigned char)(255.0f - 18.0f * t);

        unsigned char alpha = (unsigned char)(255.0f - 13.0f * t);

        DrawTexturePro(gif.texture,

                       (Rectangle){0, 0, (float)gif.texture.width,
                                   (float)gif.texture.height},

                       overlay,

                       (Vector2){0, 0},

                       0.0f,

                       (Color){shade, shade, shade, alpha});

        if (t > 0.001f) {

          int reflection = (int)(18.0f * t);

          int edge = (int)(30.0f * t);

          float edgeH = fmaxf(2.0f, overlay.height * 0.035f);

          DrawRectangleGradientV(
              (int)overlay.x, (int)overlay.y, (int)overlay.width,
              (int)(overlay.height * 0.30f),

              (Color){255, 255, 255, (unsigned char)reflection},

              BLANK);

          DrawRectangleGradientV((int)overlay.x, (int)overlay.y,
                                 (int)overlay.width, (int)edgeH,

                                 (Color){0, 0, 0, (unsigned char)edge},

                                 BLANK);

          DrawRectangleGradientV((int)overlay.x,

                                 (int)(overlay.y + overlay.height - edgeH),

                                 (int)overlay.width, (int)edgeH,

                                 BLANK,

                                 (Color){0, 0, 0, (unsigned char)edge});
        }
      }

      EndMode2D();

      if (gif.loaded) {

        overlayScreen = image_to_screen(overlay, camera);

        DrawRectangleLinesEx(overlayScreen, 2.0f, (Color){82, 181, 255, 255});

        Vector2 tl = {overlayScreen.x, overlayScreen.y};

        Vector2 tr = {overlayScreen.x + overlayScreen.width,

                      overlayScreen.y};

        Vector2 bl = {overlayScreen.x,

                      overlayScreen.y + overlayScreen.height};

        Vector2 br = {overlayScreen.x + overlayScreen.width,

                      overlayScreen.y + overlayScreen.height};

        draw_handle(tl, 14.0f);
        draw_handle(tr, 14.0f);
        draw_handle(bl, 14.0f);
        draw_handle(br, 14.0f);
      }

      EndScissorMode();
    } else {

      const char *msg = "Drop an image here";

      int fs = 30;

      DrawText(
          msg,

          (int)(canvas.x + canvas.width * 0.5f - MeasureText(msg, fs) * 0.5f),

          (int)(canvas.y + canvas.height * 0.5f - 15),

          fs,

          (Color){125, 130, 140, 255});
    }

    /* ------------------------------------------------------------------ */
    /* Sidebar                                                            */
    /* ------------------------------------------------------------------ */

    DrawRectangleRounded(sidebar, 0.035f, 10, (Color){32, 34, 38, 255});

    DrawRectangleRoundedLinesEx(sidebar, 0.035f, 10, 1.0f,
                                (Color){50, 53, 59, 255});

    float x = sidebar.x + 22.0f;

    float y = sidebar.y + 22.0f;

    float bw = sidebar.width - 44.0f;

    DrawText("OverlayMe", (int)x, (int)y, 25, (Color){244, 245, 247, 255});

    y += 31.0f;

    DrawText("Place a GIF on an image", (int)x, (int)y, 14,
             (Color){132, 136, 144, 255});

    y += 34.0f;

    /* Photo ------------------------------------------------------------ */

    label_text("Photo", x, y);

    if (button((Rectangle){x + bw - 78, y - 8, 78, 30},

               bg.loaded ? "Change" : "Choose",

               true)) {

      char p[PATH_CAP];

      if (pick_with_zenity(p, sizeof(p), "Choose photo",
                           "Images | *.png *.jpg *.jpeg *.bmp *.tga", false)) {

        if (load_background(&bg, p)) {

          snprintf(status, sizeof(status), "Photo loaded");

          reset_view(&bg, &viewZoom, &viewCenter);

          if (gif.loaded) {

            set_default_overlay(&overlay, &bg, &gif);
          }
        } else {

          snprintf(status, sizeof(status), "Could not open photo");
        }
      }
    }

    y += 22.0f;

    draw_filename(bg.loaded ? bg.path : NULL, x, y, bw);

    y += 34.0f;

    /* GIF -------------------------------------------------------------- */

    label_text("GIF", x, y);

    if (button((Rectangle){x + bw - 78, y - 8, 78, 30},

               gif.loaded ? "Change" : "Choose",

               true)) {

      char p[PATH_CAP];

      if (pick_with_zenity(p, sizeof(p), "Choose GIF", "GIF | *.gif", false)) {

        if (load_gif(&gif, p)) {

          snprintf(status, sizeof(status), "GIF loaded");

          if (bg.loaded) {

            set_default_overlay(&overlay, &bg, &gif);
          }
        } else {

          snprintf(status, sizeof(status), "Could not open GIF");
        }
      }
    }

    y += 22.0f;

    draw_filename(gif.loaded ? gif.path : NULL, x, y, bw);

    y += 30.0f;

    divider(x, y, bw);

    y += 18.0f;

    /* Controls --------------------------------------------------------- */

    if (bg.loaded && gif.loaded) {

      char a[32];
      char b[32];
      char c[32];
      char d[32];
      char zoomText[32];

      DrawText("Position", (int)x, (int)y, 17, (Color){229, 231, 234, 255});

      y += 27.0f;

      snprintf(a, sizeof(a), "%.0f", overlay.x);

      snprintf(b, sizeof(b), "%.0f", overlay.y);

      snprintf(c, sizeof(c), "%.0f", overlay.width);

      snprintf(d, sizeof(d), "%.0f", overlay.height);

      float gap = 8.0f;

      float half = (bw - gap) * 0.5f;

      value_box((Rectangle){x, y, half, 50}, "X", a);

      value_box((Rectangle){x + half + gap, y, half, 50}, "Y", b);

      y += 58.0f;

      value_box((Rectangle){x, y, half, 50}, "Width", c);

      value_box((Rectangle){x + half + gap, y, half, 50}, "Height", d);

      y += 68.0f;

      DrawText("View", (int)x, (int)y, 17, (Color){229, 231, 234, 255});

      y += 27.0f;

      snprintf(zoomText, sizeof(zoomText), "%.0f%%", viewZoom * 100.0f);

      float small = 44.0f;

      if (button((Rectangle){x, y, small, 36}, "-", true)) {

        viewZoom = fmaxf(viewZoom / 1.20f, 0.10f);
      }

      DrawRectangleRounded(
          (Rectangle){x + small + gap, y, bw - 2 * small - 2 * gap, 36},

          0.16f, 6,

          (Color){27, 29, 33, 255});

      int zw = MeasureText(zoomText, 15);

      DrawText(zoomText,

               (int)(x + small + gap + (bw - 2 * small - 2 * gap - zw) * 0.5f),

               (int)y + 10,

               15,

               (Color){222, 225, 229, 255});

      if (button((Rectangle){x + bw - small, y, small, 36}, "+", true)) {

        viewZoom = fminf(viewZoom * 1.20f, 64.0f);
      }

      y += 44.0f;

      if (button((Rectangle){x, y, half, 34}, "Fit", true)) {

        reset_view(&bg, &viewZoom, &viewCenter);
      }

      if (button((Rectangle){x + half + gap, y, half, 34}, "100%", true)) {

        float fs = fit_scale((float)bg.width, (float)bg.height, canvas);

        if (fs > 0.0f)
          viewZoom = 1.0f / fs;
      }

      y += 52.0f;

      DrawText("Preview", (int)x, (int)y, 17, (Color){229, 231, 234, 255});

      y += 27.0f;

      if (button((Rectangle){x, y, half, 36},

                 gif.playing ? "Pause" : "Play",

                 true)) {

        gif.playing = !gif.playing;
      }

      if (button((Rectangle){x + half + gap, y, half, 36},

                 "Reset", true)) {

        set_default_overlay(&overlay, &bg, &gif);
      }

      y += 54.0f;

      DrawText("Blend-In", (int)x, (int)y, 14, (Color){190, 194, 201, 255});

      char realismText[16];

      snprintf(realismText, sizeof(realismText), "%.0f%%", realism * 100.0f);

      DrawText(realismText,

               (int)(x + bw - MeasureText(realismText, 14)),

               (int)y,

               14,

               (Color){145, 149, 157, 255});

      y += 24.0f;

      realism = slider((Rectangle){x, y, bw, 16}, realism);

      float hintY = sidebar.y + sidebar.height - 142.0f;

      DrawText("Wheel to zoom  |  drag to move", (int)x, (int)hintY, 13,
               (Color){116, 120, 128, 255});

      DrawText("Drag corners to resize", (int)x, (int)hintY + 19, 13,
               (Color){116, 120, 128, 255});

      divider(x, sidebar.y + sidebar.height - 95.0f, bw);

      Rectangle exportBtn = {x, sidebar.y + sidebar.height - 70.0f, bw, 46.0f};

      if (primary_button(exportBtn, "Export", true)) {

        char out[PATH_CAP];

        if (!pick_with_zenity(out, sizeof(out), "Save animated PNG",
                              "PNG | *.png", true)) {

          snprintf(out, sizeof(out), "overlay.png");
        }

        if (!has_ext_ci(out, ".png")) {

          size_t n = strlen(out);

          if (n + 4 < sizeof(out)) {

            strcat(out, ".png");
          }
        }

        snprintf(status, sizeof(status), "Exporting...");

        EndDrawing();

        int rc = export_apng(bg.path, gif.path, overlay, realism, out);

        if (rc == 0) {

          snprintf(status, sizeof(status), "Saved %s", GetFileName(out));
        } else if (rc == 127) {

          snprintf(status, sizeof(status), "FFmpeg not found");
        } else {

          snprintf(status, sizeof(status), "Export failed");
        }

        continue;
      }
    } else {

      float cy = y + 12.0f;

      DrawText("Drop files onto the window", (int)x, (int)cy, 16,
               (Color){194, 198, 204, 255});

      DrawText("or choose them above.", (int)x, (int)cy + 23, 14,
               (Color){124, 128, 136, 255});
    }

    /* Mouse position --------------------------------------------------- */

    if (bg.loaded && CheckCollisionPointRec(GetMousePosition(), canvas)) {

      Vector2 ip = GetScreenToWorld2D(GetMousePosition(), camera);

      if (ip.x >= 0 && ip.y >= 0 && ip.x < bg.width && ip.y < bg.height) {

        char mouseInfo[96];

        snprintf(mouseInfo, sizeof(mouseInfo),

                 "%.0f, %.0f  ·  %.0f%%",

                 floorf(ip.x), floorf(ip.y), viewZoom * 100.0f);

        int tw = MeasureText(mouseInfo, 15);

        DrawRectangle((int)(canvas.x + canvas.width - tw - 26),

                      (int)(canvas.y + 10),

                      tw + 16,

                      25,

                      (Color){10, 12, 15, 205});

        DrawText(mouseInfo,

                 (int)(canvas.x + canvas.width - tw - 18),

                 (int)(canvas.y + 15),

                 15,

                 (Color){220, 224, 230, 255});
      }
    }

    EndDrawing();
  }

  /* ---------------------------------------------------------------------- */
  /* Cleanup                                                                */
  /* ---------------------------------------------------------------------- */

  unload_gif(&gif);
  unload_background(&bg);

  CloseWindow();

  return 0;
}
