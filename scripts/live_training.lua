-- Owner-thread adapter. Every value comes from the parsed live session.
local queued, serial, snapshot, talkCallback = {}, 0, nil, nil
local runtime = UHRuntime.new(uh_now, function(ms, fn)
  serial = serial + 1
  queued[#queued+1] = {at=uh_now()+ms, fn=fn, id=serial}
end, uh_error)
local api = {
  online=function() return snapshot and snapshot.online end,
  name=function() return snapshot.name end,
  say=uh_say,
  manaPercent=function() return snapshot.manaPercent end,
  inPz=function() return snapshot.inPz end,
  attacking=function() return snapshot.attacking end,
  playerId=function() return snapshot.playerId end,
  attackId=function() return snapshot.attackId end,
  followId=function() return snapshot.followId end,
  follow=uh_follow,
  cancel=uh_cancel,
  position=function() return snapshot.position end,
  spectators=function() return snapshot.spectators end,
  attack=uh_attack,
  onTalk=function(fn) talkCallback=fn;return function() talkCallback=nil end end,
}
snapshot = uh_snapshot()
local training = UHTraining.start(api,runtime)
uh_live = {}
function uh_live.tick()
  snapshot=uh_snapshot()
  local now, due, pending = uh_now(), {}, {}
  for _,task in ipairs(queued) do
    if task.at<=now then due[#due+1]=task else pending[#pending+1]=task end
  end
  queued=pending
  table.sort(due,function(a,b) return a.at<b.at or a.at==b.at and a.id<b.id end)
  for _,task in ipairs(due) do task.fn() end
end
function uh_live.talk(name,level,mode,text)
  snapshot=uh_snapshot()
  if talkCallback then talkCallback(name,level,mode,text) end
end
function uh_live.stop()
  training:stop();runtime:stop();queued={};snapshot=nil
end
