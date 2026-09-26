-- Wilds DualSense BT Rumble - game side.
--
-- Wilds plays rumble through the DualSense "advanced" haptic path, which rides the
-- controller's USB audio endpoint and is therefore silent over Bluetooth. The game
-- still resolves Capcom's ordinary motor waveform first and hands it to
-- tryADVibration, so we read it there - observing only, changing nothing - replay
-- it on our own clock, and publish the current motor levels for the plugin to send
-- to the pad.
--
-- Note: lua file IO here is sandboxed to reframework/data, so paths are bare names.

local function try(f, ...) local ok, r = pcall(f, ...) if ok then return r end return nil end

local GENERIC     = "ace.PadVibrationManager`1<app.cADVibration>"
-- Settings and the live motor levels share one file, under separate keys. This is
-- the same filename v1.0 kept its settings in, so updating needs nothing moved:
-- the settings are read back out of it whichever shape they are in.
local STATE_FILE  = "WildsDualSenseBTRumble.json"
local LEGACY_TXT  = "WildsDualSenseBTRumble.txt"    -- v1.0 levels file, now unused
local MOTOR_COUNT = 4                 -- LOW, HIGH, LTRIGGER, RTRIGGER
local MAX_VOICES  = 48

-- The only line that differs between the English and Korean releases. It is not
-- part of the saved settings on purpose: otherwise a config left behind by one
-- release would override the language of the other.
local LANGUAGE    = "en"              -- "en" or "ko"

local settings = {
    enabled   = true,
    strength  = 0.50,     -- ceiling: what the very strongest event is allowed to reach
    gamma     = 1.90,     -- >1 pushes weak rumble toward silence and keeps hits punchy
    -- Ignore authored entries weaker than this. Wilds fires a lot of very light
    -- rumble, and leaving it in merges into a continuous drone. Raising this is
    -- the only control that reduces how much of the time the motors run: measured
    -- in combat, 0.30 cut that from 38% to 21%. Kept low by default so the subtle
    -- feedback Capcom authored is not thrown away.
    gate      = 0.15,
    lowScale  = 1.0,      -- heavy motor
    highScale = 1.0,      -- snappy motor
    -- Hard floor, to stop a decaying tail from leaving the motors humming at a
    -- level nobody can feel. Keep it just above what rounds to a zero byte:
    -- higher values silently eat ordinary hits at low strength settings and turn
    -- the bottom of the slider into a dead zone.
    minOutput = 0.004,
}

local voices   = {}
local level    = {}
local lastSent = {}
local sequence, writes, decoded, failures, calls, gated = 0, 0, 0, 0, 0, 0
-- Evidence-gathering for the map-transition rumble report: separate from the
-- above so a long play session doesn't need the log file to see whether it is
-- a hitch clearing voices, a genuinely long authored preset, or neither.
local hitches, longStarts = 0, 0
local lastHitch, lastLong  = "(none yet)", "(none yet)"
local diag = {}

for i = 0, MOTOR_COUNT - 1 do level[i] = 0.0; lastSent[i] = -1.0 end

local function say(s) diag[#diag+1] = s; log.info("[btr] " .. s) end

----------------------------------------------------------------------
-- Settings persistence
----------------------------------------------------------------------
-- Fixed, so the file reads the same way every time it is written: pairs() would
-- shuffle the lines around on every frame.
local SETTING_ORDER = { "enabled", "strength", "gamma", "gate",
                        "lowScale", "highScale", "minOutput" }

local function applySetting(k, raw)
    local cur = settings[k]
    if cur == nil then return false end
    if type(cur) == "boolean" then
        settings[k] = (raw == true or raw == "true" or raw == 1 or raw == "1")
    else
        settings[k] = tonumber(raw) or cur
    end
    return true
end

-- Last resort. The file is rewritten by truncating it, so a crash caught mid-write
-- leaves a prefix - valid settings text inside a json document with no closing
-- brace, which a strict parser throws out whole. Reading the pairs off the lines
-- gets them back. It also forgives a stray comma from hand-editing.
local function scavengeSettings()
    local f = try(io.open, STATE_FILE, "r")
    if not f then return end
    for line in f:lines() do
        local k, v = line:match('"([%a_][%w_]*)"%s*:%s*([^,%s}]+)')
        if k then applySetting(k, v) end
    end
    f:close()
end

local function loadSettings()
    local saved = try(json.load_file, STATE_FILE)
    if type(saved) ~= "table" then return scavengeSettings() end

    -- v1.1 keeps the settings under their own key, beside the live state. v1.0
    -- wrote them bare at the top level. Both are read, so an update carries over
    -- whatever the player had dialled in and the file needs no migrating.
    local from = type(saved.settings) == "table" and saved.settings or saved
    for k in pairs(settings) do
        if from[k] ~= nil then applySetting(k, from[k]) end
    end
end

-- v1.0 published the levels in a file of their own. Nothing reads it now, so it
-- would just sit there after an update. The plugin clears it too, in case
-- os.remove is not one of the things this sandbox allows.
local function removeLegacyFile() try(os.remove, LEGACY_TXT) end

loadSettings()
removeLegacyFile()

----------------------------------------------------------------------
-- Clock
----------------------------------------------------------------------
local clockNow  = try(function() return os.clock() end)
local hasClock  = (clockNow ~= nil)
local lastClock = clockNow or 0

-- Second return value is true on a big gap (a loading screen or a real stall):
-- the caller drops every active voice rather than decaying it, because pretending
-- only 1/60s passed is what used to make a rumble that was mid-flight when the
-- stall began keep buzzing throughout it - the gap is stale time, not slow time.
-- Third return value is the raw gap, for logging only.
local function delta()
    if not hasClock then return 1.0 / 60.0, false, 0.0 end
    local now = try(function() return os.clock() end) or lastClock
    local dt = now - lastClock
    lastClock = now
    if dt <= 0.0 then return 1.0 / 60.0, false, dt end
    if dt > 0.25 then return 1.0 / 60.0, true, dt end
    return dt, false, dt
end

----------------------------------------------------------------------
-- Read a preset and start one voice per motor entry
----------------------------------------------------------------------
local PRESET_TYPE = "ace.user_data.PadVibrationPresetList.cPadVibrationPreset"

-- The hook fires thousands of times a session and the argument is not always a
-- preset. Reading fields off something that is not one is an access violation in
-- native code, which pcall cannot catch, so the type is confirmed first.
local function isPreset(obj)
    if obj == nil then return false end
    local td = try(function() return obj:get_type_definition() end)
    if td == nil then return false end
    local name = try(function() return td:get_full_name() end)
    return name == PRESET_TYPE
end

local function startVoices(preset)
    local mv = try(function() return preset:get_field("_MotorVibration") end)
    local n  = mv and try(function() return mv:get_size() end) or 0
    if n == 0 or n > 64 then return false end

    local started = 0
    local maxDur, maxPower, maxMotor = 0.0, 0.0, -1
    for j = 0, n - 1 do
        local e = try(function() return mv:get_element(j) end)
        if e then
            local motor = tonumber(try(function() return e:get_field("_MotorType") end)) or -1
            local power = tonumber(try(function() return e:get_field("_Power") end)) or 0.0
            local dur   = tonumber(try(function() return e:get_field("_Time") end)) or 0.0
            local atten = try(function() return e:get_field("_IsTimeAttenuation") end) and true or false

            if motor >= 0 and motor < MOTOR_COUNT and power > 0.0 and dur > 0.0 then
                -- Weak entries fire constantly. Left in, they merge into one long
                -- drone that reads as "too strong" however far the level is turned
                -- down, so they are dropped rather than scaled.
                if power < settings.gate then
                    gated = gated + 1
                elseif #voices < MAX_VOICES then
                    voices[#voices + 1] = { motor = motor, power = power, dur = dur, t = 0.0, atten = atten }
                    started = started + 1
                    if dur > maxDur then maxDur, maxPower, maxMotor = dur, power, motor end
                end
            end
        end
    end
    -- Tells a genuinely long authored preset (map-transition ambience, quest
    -- clear) apart from a stale voice lingering past when it should have ended.
    if maxDur >= 1.0 then
        longStarts = longStarts + 1
        lastLong = string.format("motor %d  power %.2f  dur %.2fs", maxMotor, maxPower, maxDur)
        log.info("[btr] long preset: " .. lastLong)
    end
    return started > 0
end

----------------------------------------------------------------------
-- Observe. This never changes what the game does.
----------------------------------------------------------------------
do
    local td = try(sdk.find_type_definition, GENERIC)
    say("manager type: " .. tostring(td ~= nil))
    local hooked = 0
    for _, m in ipairs((td and try(function() return td:get_methods() end)) or {}) do
        if try(function() return m:get_name() end) == "tryADVibration" then
            if pcall(sdk.hook, m, function(args)
                if not settings.enabled then return end
                calls = calls + 1
                local raw = args[3]
                if raw == nil or try(sdk.to_int64, raw) == 0 then failures = failures + 1 return end
                local preset = try(sdk.to_managed_object, raw)
                if isPreset(preset) and startVoices(preset) then
                    decoded = decoded + 1
                else
                    failures = failures + 1
                end
            end, function(r) return r end) then hooked = hooked + 1 end
        end
    end
    say("tryADVibration hooks: " .. hooked)
end

----------------------------------------------------------------------
-- Mix the active voices and publish
----------------------------------------------------------------------
-- Written by hand rather than through json.dump_file: this runs every frame, and
-- string.format costs nothing next to building a table and handing it to the
-- serialiser. %.6g keeps the settings readable and free of exponents, and is the
-- same text whichever Lua version is underneath.
local function jsonValue(v)
    if type(v) == "boolean" then return v and "true" or "false" end
    return string.format("%.6g", v)
end

-- Both halves go out together, every frame. The levels have to be written that
-- often anyway, and the settings cost a few dozen bytes on top of a write that
-- was already happening - cheaper than keeping a second file in step.
local function writeState()
    local f = try(io.open, STATE_FILE, "w")
    if not f then return false end
    sequence = (sequence + 1) % 1000000

    -- Settings first. The file is rewritten by truncating, so a reader that
    -- catches it mid-write sees a prefix; keeping the settings at the front keeps
    -- them clear of that window.
    f:write('{\n  "settings": {\n')
    for i, k in ipairs(SETTING_ORDER) do
        f:write(string.format('    "%s": %s%s\n', k, jsonValue(settings[k]),
                              i < #SETTING_ORDER and "," or ""))
    end
    f:write("  },\n")

    -- The state stays on one line on purpose. The plugin takes the last line that
    -- carries all of its fields, so it either gets this whole object or rejects
    -- it - it can never pair a fresh sequence number with a stale level.
    f:write(string.format(
        '  "state": {"sequence": %d, "low": %.4f, "high": %.4f, "ltrigger": %.4f, "rtrigger": %.4f}\n}\n',
        sequence, level[0], level[1], level[2], level[3]))

    f:close()
    writes = writes + 1
    for i = 0, MOTOR_COUNT - 1 do lastSent[i] = level[i] end
    return true
end

-- Settings live in the same file, so saving them is just writing it. Done here
-- rather than waiting for the next frame: with the mod disabled nothing else
-- writes, and the change would otherwise be lost on exit.
local function saveSettings() writeState() end

local function changed()
    for i = 0, MOTOR_COUNT - 1 do
        if math.abs(level[i] - lastSent[i]) > 0.002 then return true end
    end
    return false
end

local function scaleFor(motor)
    if motor == 0 then return settings.strength * settings.lowScale end
    if motor == 1 then return settings.strength * settings.highScale end
    return settings.strength
end

local sinceWrite = 0

re.on_frame(function()
    local dt, hitch, rawDt = delta()

    for i = 0, MOTOR_COUNT - 1 do level[i] = 0.0 end

    if hitch then
        hitches = hitches + 1
        lastHitch = string.format("gap %.2fs, cleared %d voice(s)", rawDt, #voices)
        log.info("[btr] hitch: " .. lastHitch)
        voices = {}
    end

    local keep = {}
    for _, v in ipairs(voices) do
        v.t = v.t + dt
        if v.t < v.dur then
            -- The curve is applied to the authored power, not the final level, so
            -- turning the strength down keeps the contrast between a tap and a hit
            -- instead of flattening everything into one buzz.
            local base = v.power ^ settings.gamma
            -- Capcom marks the entries that should fade out over their lifetime.
            local power = v.atten and (base * (1.0 - v.t / v.dur)) or base
            power = power * scaleFor(v.motor)
            if power > 1.0 then power = 1.0 end
            if power > level[v.motor] then level[v.motor] = power end
            keep[#keep + 1] = v
        end
    end
    voices = keep

    for i = 0, MOTOR_COUNT - 1 do
        if level[i] < settings.minOutput then level[i] = 0.0 end
    end

    -- Heartbeat, so the plugin can tell a held value from a dead game.
    sinceWrite = sinceWrite + 1
    if settings.enabled and (changed() or sinceWrite >= 30) then
        writeState()
        sinceWrite = 0
    end
end)

re.on_script_reset(function()
    voices = {}
    for i = 0, MOTOR_COUNT - 1 do level[i] = 0.0 end
    writeState()
end)

----------------------------------------------------------------------
-- UI
----------------------------------------------------------------------
local TEXT = {
    en = {
        enabled   = "Enabled",
        strength  = "Vibration strength",
        advanced  = "Advanced settings",
        note      = "※ Set it low enough and the faintest rumble may not come through at all.",
        gamma     = "Contrast (gamma)",
        gate      = "Gate (drop weak rumble)",
        low       = "Heavy motor (LOW)",
        high      = "Snappy motor (HIGH)",
        recommend = "Reset",
        test      = "Test buzz",
        clear     = "Clear",
        requests  = "requests      : %d  decoded %d  skipped %d",
        gatedOut  = "gated out     : %d",
        voices    = "active voices : %d",
        written   = "lines written : %d",
        output    = "OUTPUT  LOW %.3f   HIGH %.3f",
        hitches   = "hitches       : %d  (%s)",
        longs     = "long presets  : %d  (%s)",
    },
    ko = {
        enabled   = "사용",
        strength  = "진동 세기",
        advanced  = "고급 설정",
        note      = "※ 너무 낮추면 약한 진동은 아예 전달되지 않을 수 있습니다.",
        gamma     = "강약 대비 (GAMMA)",
        gate      = "약한 진동 차단 (GATE)",
        low       = "저주파 (LOW)",
        high      = "고주파 (HIGH)",
        recommend = "리셋",
        test      = "진동 테스트",
        clear     = "초기화",
        requests  = "진동 요청   : %d  해석 %d  건너뜀 %d",
        gatedOut  = "차단된 진동 : %d",
        voices    = "재생 중     : %d",
        written   = "전송 횟수   : %d",
        output    = "출력  저주파 %.3f   고주파 %.3f",
        hitches   = "끊김 감지   : %d  (%s)",
        longs     = "긴 진동     : %d  (%s)",
    },
}

local t = TEXT[LANGUAGE] or TEXT.en

re.on_draw_ui(function()
    if not imgui.tree_node("Wilds DualSense BT Rumble") then return end

    local dirty = false
    local ch, v

    ch, v = imgui.checkbox(t.enabled, settings.enabled)
    if ch then
        settings.enabled = v
        dirty = true
        if not v then
            voices = {}
            for i = 0, MOTOR_COUNT - 1 do level[i] = 0.0 end
            writeState()
        end
    end

    imgui.text("")

    -- Shown as a percentage, where 100% is what the game itself asked for. The
    -- value behind it is still a plain multiplier, capped at the game's own peak:
    -- going above it cannot raise the loudest events, which are already at the
    -- hardware ceiling, so it would only flatten the range.
    ch, v = imgui.slider_float(t.strength, settings.strength * 100.0, 0.0, 100.0, "%.0f%%")
    if ch then settings.strength = v / 100.0; dirty = true end

    imgui.same_line()
    if imgui.button(t.test) then
        voices[#voices + 1] = { motor = 0, power = 1.0, dur = 0.4, t = 0.0, atten = true }
        voices[#voices + 1] = { motor = 1, power = 1.0, dur = 0.4, t = 0.0, atten = true }
    end

    imgui.text(t.note)
    imgui.text("")

    if imgui.tree_node(t.advanced) then
        ch, v = imgui.slider_float(t.gamma, settings.gamma, 1.0, 4.0, "%.2f")
        if ch then settings.gamma = v; dirty = true end

        ch, v = imgui.slider_float(t.gate, settings.gate, 0.0, 0.5, "%.2f")
        if ch then settings.gate = v; dirty = true end

        imgui.text("")
        ch, v = imgui.slider_float(t.low, settings.lowScale, 0.0, 2.0, "%.2f")
        if ch then settings.lowScale = v; dirty = true end

        ch, v = imgui.slider_float(t.high, settings.highScale, 0.0, 2.0, "%.2f")
        if ch then settings.highScale = v; dirty = true end

        if imgui.button(t.recommend) then
            settings.strength = 0.50; settings.gamma = 1.90; settings.gate = 0.15
            settings.lowScale = 1.0;  settings.highScale = 1.0
            dirty = true
        end

        imgui.text("")
        for _, l in ipairs(diag) do imgui.text(l) end
        imgui.text(string.format(t.requests, calls, decoded, failures))
        imgui.text(string.format(t.gatedOut, gated))
        imgui.text(string.format(t.voices, #voices))
        imgui.text(string.format(t.written, writes))
        imgui.text(string.format(t.output, level[0], level[1]))
        imgui.text(string.format(t.hitches, hitches, lastHitch))
        imgui.text(string.format(t.longs, longStarts, lastLong))

        if imgui.button(t.clear) then
            calls = 0; decoded = 0; failures = 0; gated = 0; writes = 0
            hitches = 0; longStarts = 0
            lastHitch = "(none yet)"; lastLong = "(none yet)"
        end

        imgui.tree_pop()
    end

    if dirty then saveSettings() end

    imgui.tree_pop()
end)
