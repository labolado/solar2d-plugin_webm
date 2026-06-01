display.setStatusBar(display.HiddenStatusBar)

local webm = require("plugin.webm")

local W, H   = display.contentWidth, display.contentHeight
local CX, CY = display.contentCenterX, display.contentCenterY

-- Normal mode: 16:9 box centered
local NW = math.floor(W * 0.85)
local NH = math.floor(NW * 9 / 16)
local NY = CY - 30

-- Fullscreen mode: fill width, maintain 16:9 (letterboxed)
local FW = W
local FH = math.floor(W * 9 / 16)

local isFullscreen = false
local movie

-- ── Background ──────────────────────────────────────────────────────────────
local bg = display.newRect(CX, CY, W, H)
bg:setFillColor(0.1, 0.1, 0.1)

-- ── Create movie once ───────────────────────────────────────────────────────
local function newMovie(w, h, y)
    if movie then movie:stop(); movie = nil end
    movie = webm.newMovieRect({
        filename = "sample4k.webm",
        baseDir  = system.ResourceDirectory,
        x = CX, y = y,
        width = w, height = h,
        loop = true,
    })
    movie:play()
end

newMovie(NW, NH, NY)

-- ── HUD texts ───────────────────────────────────────────────────────────────
local fpsText    = display.newText("FPS:--",    CX, 16, native.systemFont, 14)
local statusText = display.newText("playing",   CX, 36, native.systemFont, 13)
local timeText   = display.newText("0.00s",     CX, H - 18, native.systemFont, 13)
fpsText:setFillColor(1, 0.3, 0.3)

-- ── Button strip ────────────────────────────────────────────────────────────
local controls = display.newGroup()
local BTN_Y = H - 52
local BTN_H = 44
local BTN_W = math.floor((W - 16) / 5)

local function makeBtn(label, idx, color, onTap)
    local x = 8 + (idx - 0.5) * BTN_W
    local r = display.newRect(controls, x, BTN_Y, BTN_W - 3, BTN_H)
    r:setFillColor(unpack(color))
    display.newText(controls, label, x, BTN_Y, native.systemFontBold, 13)
    r:addEventListener("tap", function() onTap(); return true end)
end

makeBtn("Load",  1, {0.2, 0.4, 0.8}, function()
    newMovie(NW, NH, NY)
    statusText.text = "playing"
end)
makeBtn("Play",  2, {0.1, 0.6, 0.2}, function()
    if movie then movie:play(); statusText.text = "playing" end
end)
makeBtn("Pause", 3, {0.6, 0.6, 0.1}, function()
    if movie then movie:pause(); statusText.text = "paused" end
end)
makeBtn("Stop",  4, {0.7, 0.15, 0.15}, function()
    if movie then movie:stop(); movie = nil; statusText.text = "stopped" end
end)
makeBtn("[ ]",   5, {0.25, 0.25, 0.6}, function()
    isFullscreen = true
    newMovie(FW, FH, CY)
    controls.isVisible  = false
    fpsText.isVisible   = false
    statusText.isVisible = false
    timeText.isVisible  = false
    exitR.isVisible     = true
    exitT.isVisible     = true
    exitR:toFront(); exitT:toFront()
end)

-- ── Exit fullscreen button (top-right, hidden until fullscreen) ──────────────
exitR = display.newRoundedRect(W - 40, 28, 68, 32, 8)
exitR:setFillColor(0, 0, 0, 0.7)
exitT = display.newText("[X] Exit", W - 40, 28, native.systemFontBold, 12)
exitR.isVisible = false
exitT.isVisible = false

exitR:addEventListener("tap", function()
    isFullscreen = false
    newMovie(NW, NH, NY)
    controls.isVisible   = true
    fpsText.isVisible    = true
    statusText.isVisible = true
    timeText.isVisible   = true
    exitR.isVisible      = false
    exitT.isVisible      = false
    statusText.text = "playing"
    return true
end)

-- ── Single enterFrame listener ───────────────────────────────────────────────
local frameCount, lastT = 0, system.getTimer()
Runtime:addEventListener("enterFrame", function()
    frameCount = frameCount + 1
    local t = system.getTimer()
    if t - lastT >= 1000 then
        fpsText.text = string.format("FPS:%d  Lua:%dKB",
            frameCount, math.floor(collectgarbage("count")))
        frameCount = 0; lastT = t
    end
    if movie then
        timeText.text = string.format("%.2fs", movie:getCurrentTime())
    end
    if isFullscreen then
        exitR:toFront(); exitT:toFront()
    end
end)
