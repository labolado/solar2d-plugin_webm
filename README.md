# plugin.webm

A native [Solar2D](https://solar2d.com) plugin that decodes **WebM** video
(VP8/VP9 + Opus audio) into a Corona external texture, so you can play video
inside your scene like any other display object.

- **Video:** VP8 / VP9
- **Audio:** Opus
- **Container:** WebM (`.webm`)
- **Platforms:** macOS, iOS, tvOS, Android, Win32

## Install

Add the plugin to `build.settings`:

```lua
-- Change "v1" to the latest release tag
local webm_base = "https://github.com/labolado/solar2d-plugin_webm/releases/download/v1/"

settings = {
    plugins = {
        ["plugin.webm"] = {
            publisherId = "com.labolado",
            supportedPlatforms = {
                ["mac-sim"]    = { url = webm_base .. "2025.3720-mac-sim.tgz" },
                ["android"]    = { url = webm_base .. "2025.3720-android.tgz" },
                ["iphone"]     = { url = webm_base .. "2025.3720-iphone.tgz" },
                ["iphone-sim"] = { url = webm_base .. "2025.3720-iphone-sim.tgz" },
                ["win32-sim"]  = { url = webm_base .. "2025.3720-win32-sim.tgz" },
                ["appletvos"]  = { url = webm_base .. "2025.3720-appletvos.tgz" },
            },
        },
    },
}
```

Then require it:

```lua
local webm = require("plugin.webm")
```

## Quick start — `newMovieRect`

The easy path. `newMovieRect` returns a self-driving display object (a rect with
the video as its fill); it advances itself every frame once you call `play()`.

```lua
local webm = require("plugin.webm")

local movie = webm.newMovieRect({
    filename = "sample.webm",
    baseDir  = system.ResourceDirectory,
    x = display.contentCenterX,
    y = display.contentCenterY,
    width  = 320,   -- on-screen size; the texture is decoded at this size
    height = 180,
    loop   = true,
})

movie:play()
```

### `newMovieRect(opts)` options

| key          | type     | default                     | description                                            |
|--------------|----------|-----------------------------|--------------------------------------------------------|
| `filename`   | string   | —                           | WebM file (required)                                   |
| `baseDir`    | constant | `system.ResourceDirectory`  | base directory for `filename`                          |
| `width`      | number   | —                           | on-screen width (also caps decode resolution)          |
| `height`     | number   | —                           | on-screen height                                       |
| `x`, `y`     | number   | `0`                         | position                                               |
| `loop`       | bool     | `false`                     | restart automatically at end                           |
| `preserve`   | bool     | `false`                     | keep the object alive after `stop()` instead of disposing |
| `listener`   | function | —                           | called with `{ name="movie", phase="stopped", completed=<bool> }` |

### `newMovieRect` methods

```lua
movie:play()              -- start / resume
movie:pause()             -- pause (keeps position)
movie:stop()              -- stop (fires the listener; disposes unless preserve=true)
movie:reset()             -- rewind to the start, keep playing
movie:seek(t)             -- jump to time t (seconds); works while playing or paused
local t = movie:getCurrentTime()  -- current position in seconds
movie:dispose()           -- release the texture and remove the object
```

The object is a normal display object, so `movie.x`, `movie:translate(...)`,
inserting into a group, etc. all work.

## Lower-level — `newMovieTexture`

If you want full control (custom display object, your own frame loop), create the
texture directly and drive it yourself.

```lua
local texture = webm.newMovieTexture({
    filename = "sample.webm",
    baseDir  = system.ResourceDirectory,
    width    = 320,   -- decode/upload cap; omit (or fullResolution=true) for native size
    height   = 180,
})

local rect = display.newImageRect(texture.filename, texture.baseDir, 320, 180)
rect.x, rect.y = display.contentCenterX, display.contentCenterY

texture:play()

-- Drive playback every frame:
local last = system.getTimer()
Runtime:addEventListener("enterFrame", function()
    local now = system.getTimer()
    local delta = now - last
    last = now
    if texture.isActive then
        texture:update(delta)   -- advance by delta milliseconds
        texture:invalidate()    -- mark the texture dirty
    end
end)
```

### Texture methods & properties

| member                | description                                           |
|-----------------------|-------------------------------------------------------|
| `texture:play()`      | start / resume playback                               |
| `texture:pause()`     | pause                                                 |
| `texture:stop()`      | stop and tear down audio                              |
| `texture:replay()`    | rewind to the start                                   |
| `texture:seek(t)`     | jump to time `t` (seconds)                            |
| `texture:update(ms)`  | advance playback by `ms` milliseconds                 |
| `texture:invalidate()`| mark the texture dirty after `update`                 |
| `texture:setVolume(v)`| audio gain, `0.0`–`1.0`                               |
| `texture.isActive`    | `false` once playback has finished                    |
| `texture.isPlaying`   | currently playing                                     |
| `texture.currentTime` | playback position in seconds                          |
| `texture.volume`      | current volume (`0.0`–`1.0`)                          |
| `texture:releaseSelf()` | free the texture (when you no longer need it)       |

> **Note:** never call `texture:releaseSelf()` in the same frame the texture was
> last drawn — the renderer uploads it later in the frame, so free it on the next
> `enterFrame` instead. (`newMovieRect` handles this for you.)

## Notes

- `width`/`height` cap the decoded texture to the on-screen pixel size, so a 4K
  source shown in a small rect isn't decoded/uploaded at full resolution. Pass
  `fullResolution = true` (or omit `width`/`height`) to keep the native size.
- Video is decoded on a background thread, so loading or seeking a large clip
  doesn't block the render thread.
- Each movie owns its own audio source, so multiple movies can play at once
  without clobbering each other's audio.

## License

BSD 3-Clause — see [LICENSE](LICENSE). Bundled third-party decoders
(libvpx, libwebm, opus under `third_party/`) keep their own BSD licenses.
