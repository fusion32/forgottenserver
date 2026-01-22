local handler = PacketHandler(0xEE)

function handler.onReceive(player, msg)
	-- NOTE(fusion): Send greet message to targeted NPC to avoid conflicts with
	-- other surrounding NPCs, and to the player for feedback.
	local npc = Npc(msg:getU32())
	if npc then
		player:say("hi", TALKTYPE_SAY, false, npc)
	end
	player:say("hi", TALKTYPE_SAY, false, player)
end

handler:register()
