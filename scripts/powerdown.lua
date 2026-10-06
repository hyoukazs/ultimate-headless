local M = {}
-- api.manapercent returns nil when player/mana maximum are invalid.
function M.create(runtime, api)
  return runtime:macro(1000, function()
    if not api.isOnline() then return end
    local percent = api.manapercent()
    if type(percent) == 'number' and percent == percent and percent > 90 and percent <= 100 then api.say('powerdown') end
  end)
end
return M
