-- Package K3 (fishing): quests that ask for fish got a way to be done, so what guild bots failed on before it does not
-- count any more. Without this the two "no_progress" blocks of "The Family and the Fishing Pole" (1141) would keep it
-- "learned" (left alone by every AI-played character) although it is off the hand list (ga_quest_block_template).
-- Applied once by the DB updater; a bot that can fish and fails again is counted afresh.
DELETE FROM `ga_bot_quest_block`
WHERE `reason` = 'no_progress'
  AND `quest_id` IN (1141, 1579, 1580, 6607, 9205, 9206);
