local updateClientOnAdvanceSkill = CreatureEvent("Update Client On Advance Skill")

function updateClientOnAdvanceSkill.onAdvance(player, skill, oldLevel, newLevel)
	if skill == SKILL_LEVEL then
		return true
	end

	if newLevel > oldLevel then
		player:sendSkillUp(skill, newLevel)
	end
	return true
end

updateClientOnAdvanceSkill:register()
