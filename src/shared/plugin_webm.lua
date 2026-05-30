local Library = require('CoronaLibrary')

-- Create stub library
local lib = Library:new(
    {
        name = 'plugin.webm',
        publisherId = 'com.labolado'
    }
)

function lib.newMovieTexture(opts)
    local path = system.pathForFile(opts.filename, opts.baseDir or system.ResourceDirectory)
    -- The native side owns a private OpenAL source per movie (so movies can't
    -- clobber each other's audio), so the channel/source here is no longer used
    -- for playback. We still touch a channel's source once purely to force
    -- Corona's OpenAL context to initialise before the plugin creates its source.
    local source = audio.getSourceFromChannel(opts.channel or 1)
    -- Cap the decoded texture to the on-screen pixel size so a 4K source shown
    -- in a small rect isn't converted/uploaded at full resolution. Pass
    -- opts.fullResolution = true (or omit width/height) to keep native size.
    local maxW, maxH = 0, 0
    if not opts.fullResolution and opts.width and opts.height then
        maxW = math.ceil(opts.width * (display.pixelWidth / display.contentWidth))
        maxH = math.ceil(opts.height * (display.pixelHeight / display.contentHeight))
    end
    return lib._newMovieTexture(path, source, display.fps, maxW, maxH)
end

-- IMPORTANT: never releaseSelf() a movie texture synchronously inside the frame
-- in which it was last displayed. Corona uploads the texture later the same frame
-- (Renderer::Swap -> GLTexture::Update -> PlatformBitmapTexture::GetData), so
-- freeing it now is a use-after-free -> crash. Defer the release to the next
-- enterFrame: the display object is removed now, the texture is freed next frame.
local _pendingRelease = {}
local _flushScheduled = false

local function _flushReleases()
    for i = #_pendingRelease, 1, -1 do
        local tex = _pendingRelease[i]
        _pendingRelease[i] = nil
        if tex and type(tex.releaseSelf) == 'function' then
            tex:releaseSelf()
        end
    end
    Runtime:removeEventListener('enterFrame', _flushReleases)
    _flushScheduled = false
end

local function _releaseTexture(tex)
    if not tex then return end
    _pendingRelease[#_pendingRelease + 1] = tex
    if not _flushScheduled then
        _flushScheduled = true
        Runtime:addEventListener('enterFrame', _flushReleases)
    end
end

-- Plug-n-play movie display object.
-- opts: filename, baseDir, width, height, x, y, loop, preserve, listener, channel.
-- Methods: play / pause / stop / reset / seek(t) / getCurrentTime() / dispose.
-- Functions capture `rect` directly (not via self), so both rect:play() and
-- rect.play() work, matching the original API.
function lib.newMovieRect(opts)
    local texture = lib.newMovieTexture(opts)
    if not texture then return nil end

    local rect = display.newImageRect(texture.filename, texture.baseDir, opts.width, opts.height)
    rect.texture = texture
    rect.channel = opts.channel
    if opts.x then rect.x = opts.x end
    if opts.y then rect.y = opts.y end
    rect._preserve = opts.preserve
    rect._loop = opts.loop
    rect.listener = opts.listener

    rect._prevtime = -1
    rect._stop = false
    rect.playing = false
    rect._started = false
    rect._complete = false

    -- Per-frame driver. Stored on the instance so add/removeEventListener use the
    -- same function reference.
    rect._onFrame = function(event)
        if rect.playing and rect.texture then
            if rect._prevtime < 0 then rect._prevtime = event.time end
            local delta = event.time - rect._prevtime

            if rect.texture.isActive then
                rect.texture:update(delta)
                rect.texture:invalidate()
            else
                rect._complete = true
                if rect._loop then
                    rect.reset()
                else
                    rect.stop()
                end
            end
        end
        rect._prevtime = event.time
    end

    -- Rewind to the start (used for looping). Keeps playing.
    rect.reset = function()
        if not rect.playing or not rect.texture then return end
        rect._complete = false
        rect._prevtime = -1
        if type(rect.texture.replay) == 'function' then
            rect.texture:replay()
        end
    end

    rect.play = function()
        if rect.playing or not rect.texture then return end
        rect.texture:play()
        rect.playing = true
        if not rect._started then
            rect._started = true
            Runtime:addEventListener('enterFrame', rect._onFrame)
        end
    end

    rect.pause = function()
        if not rect.playing then return end
        rect.playing = false
        if rect.texture then rect.texture:pause() end
    end

    -- Jump to time t (seconds). Preserves play/pause state; the target frame is
    -- shown either way. Returns true on success.
    rect.seek = function(t)
        if not rect.texture or type(rect.texture.seek) ~= 'function' then return false end
        if not t or t < 0 then t = 0 end
        local ok = rect.texture:seek(t)
        rect._prevtime = -1
        rect._complete = false
        rect._stop = false
        -- Re-arm the frame loop if the movie had finished/stopped.
        if not rect._started then
            rect._started = true
            Runtime:addEventListener('enterFrame', rect._onFrame)
        end
        -- Nudge a render so a paused movie immediately shows the seeked frame.
        rect.texture:update(0)
        rect.texture:invalidate()
        return ok
    end

    -- Current playback position in seconds.
    rect.getCurrentTime = function()
        if rect.texture and rect.texture.currentTime then
            return rect.texture.currentTime
        end
        return 0
    end

    rect.stop = function()
        if rect._stop then return end
        Runtime:removeEventListener('enterFrame', rect._onFrame)
        rect.playing = false
        rect._started = false
        if rect.texture then rect.texture:stop() end
        rect._stop = true

        if rect.listener then
            rect.listener(
                {
                    name = 'movie',
                    phase = 'stopped',
                    completed = rect._complete
                }
            )
        end

        if rect._preserve then return end
        rect.dispose()
    end

    rect.dispose = function()
        if rect.playing then return end
        Runtime:removeEventListener('enterFrame', rect._onFrame)
        _releaseTexture(rect.texture)
        rect.texture = nil
        if rect.removeSelf then rect.removeSelf(rect) end
    end

    return rect
end

-- Return an instance
return lib
