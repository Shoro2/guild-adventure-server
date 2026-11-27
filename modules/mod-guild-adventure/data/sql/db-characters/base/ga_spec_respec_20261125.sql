-- mod-guild-adventure: the built-in talent templates now hold the talents a spec is played with in their level-60 part
-- (Heart Strike, Tree of Life, Circle of Healing, Frost Strike, ...; tools/gen_spec_templates.py, docs/specs.md), and 44
-- of the 46 changed their order. Every bot that plays by a changed one has its talents reset and learned again along
-- it at its next quiet moment (free, as when a template is assigned). Applied once by the DB updater; a bot on its
-- class default counts by its class (the rogue's default, Leveling: Combat, did not change). Manual (1) and the guilds'
-- own templates (from 100000) are left alone.
UPDATE `ga_bot_spec` s
JOIN `characters` c ON c.`guid` = s.`bot_guid`
SET s.`reset_pending` = 1
WHERE (s.`template_id` = 0 AND c.`class` <> 4)
   OR s.`template_id` IN (101, 102, 103, 104, 105, 201, 202, 203, 204, 205, 301, 302, 303, 304, 402, 403, 405, 501, 502,
                          503, 504, 601, 602, 603, 604, 605, 701, 702, 703, 704, 801, 802, 803, 804, 805, 901, 902, 903,
                          904, 1101, 1102, 1103, 1104, 1105);
