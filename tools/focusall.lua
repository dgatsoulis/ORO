-- ---------------------------------------------------------------------------
-- focusall.lua - make every vessel in the running scenario selectable (F3)
--
-- Some vessels are created with SetEnableFocus(false) and therefore never
-- appear in the F3 vessel list. The stock Atlantis boosters and tank both do
-- this (Atlantis_SRB.cpp:148, Atlantis_Tank.cpp:74), and other addons use the
-- same pattern for stack parts. Fine for flying - but it makes those vessels
-- impossible to select for tuning: per-class effect settings (ORO's included)
-- load for the FOCUS vessel's class, and a vessel you cannot focus is a class
-- you cannot tune.
--
-- This sweeps every vessel and re-enables focus on any that had it disabled.
-- SESSION-ONLY: vessels disable themselves again on the next scenario
-- (re)load, so re-run it after reloading. Running it twice is harmless.
-- If an addon spawns vessels later in the flight, run it again to catch them.
--
-- Usage: enable the LuaConsole module (Launchpad > Modules), open the console
-- in the sim (Ctrl+F4 > Lua console window), and type:
--     run('focusall')
-- ---------------------------------------------------------------------------

local function say (msg)
  if term ~= nil and term.out ~= nil then term.out(msg) end
  oapi.write_log('focusall: '..msg)
end

local n = vessel.get_count()
local changed = 0
for i = 0, n - 1 do
  local v = vessel.get_interface(i)
  if v ~= nil and v:get_enablefocus() == false then
    v:set_enablefocus(true)
    say('focus enabled: '..v:get_name()..' ('..v:get_classname()..')')
    changed = changed + 1
  end
end
say(changed..' of '..n..' vessel(s) made selectable')
