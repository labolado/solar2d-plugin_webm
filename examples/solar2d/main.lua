display.setStatusBar(display.HiddenStatusBar)

-- Minimal test for webm.newMovieRect
local webm = require("plugin.webm")

local background = display.newRect(display.contentCenterX, display.contentCenterY,
                                   display.contentWidth, display.contentHeight)
background:setFillColor(0.15, 0.15, 0.15)

local status = display.newText({
    text = "loading...",
    x = display.contentCenterX,
    y = 40,
    font = native.systemFont,
    fontSize = 14,
})

local movie

-- (Re)create the self-driving movie object. Stop disposes the old one, so this
-- lets us load again after a Stop.
local function loadMovie()
    if movie then
        movie:stop()
        movie = nil
    end
    movie = webm.newMovieRect({
        filename = "sample4k.webm",
        baseDir  = system.ResourceDirectory,
        x = display.contentCenterX,
        y = display.contentCenterY,
        width  = 300,
        height = 200,
        loop   = false,
        listener = function(event)
            print("movie event:", event.name, event.phase, tostring(event.completed))
        end,
    })
    movie:play()
    status.text = "playing"
end

loadMovie()

-- Simple on-screen button helper (rect + label + tap handler).
local function newButton(label, x, color, onTap)
    local btn = display.newRect(x, display.contentHeight - 70, 70, 40)
    btn:setFillColor(unpack(color))
    display.newText({
        text = label,
        x = btn.x,
        y = btn.y,
        font = native.systemFontBold,
        fontSize = 15,
    })
    btn:addEventListener("tap", function()
        onTap()
        return true
    end)
    return btn
end

local spacing = 76
local x0 = display.contentCenterX - spacing * 1.5

newButton("Load", x0, {0.2, 0.4, 0.7}, function()
    loadMovie()
end)

newButton("Play", x0 + spacing, {0.2, 0.6, 0.2}, function()
    if movie then
        movie:play()
        status.text = "playing"
    end
end)

newButton("Pause", x0 + spacing * 2, {0.6, 0.6, 0.2}, function()
    if movie then
        movie:pause()
        status.text = "paused"
    end
end)

-- stop() fires the listener (phase="stopped") and, since preserve is not set,
-- disposes the object — so null it out afterwards.
newButton("Stop", x0 + spacing * 3, {0.7, 0.2, 0.2}, function()
    if movie then
        movie:stop()
        movie = nil
        status.text = "stopped"
    end
end)

local frame = 0
local fps = display.fps
local lastTime = system.getTimer()
local memoryUsageText
local function monitorMem()
    frame = frame + 1
    local currTime = system.getTimer()
    if currTime-lastTime >= 1000 then
        lastTime = currTime
        fps = frame
        frame = 0
    end
    memoryUsageText.text = string.format("FPS:%s", fps)
end

if memoryUsageText == nil then
    memoryUsageText = display.newText("fps", display.contentCenterX, 15, native.systemoFont, 30)
    memoryUsageText.alpha = .5
    memoryUsageText:setFillColor(1, 0, 0, 1)
    Runtime:addEventListener("enterFrame", monitorMem)
end

-- Show current playback time.
local timeText = display.newText({
    text = "0.00s",
    x = display.contentCenterX,
    y = display.contentHeight - 30,
    font = native.systemFont,
    fontSize = 14,
})

Runtime:addEventListener("enterFrame", function()
    if movie then
        timeText.text = string.format("%.2fs", movie:getCurrentTime())
    end
end)
