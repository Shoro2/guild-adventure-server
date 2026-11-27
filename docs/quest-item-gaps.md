# Quest items the order planner has no source for

Generated on 2026-11-12 by `_work\research\quest-classify\item_made_by.py` from the world database and the client's
`Spell.dbc` / `SkillLineAbility.dbc`. Scope: zone quests of level 1-58 that are not repeatable, not for a group, a
dungeon, a raid or PvP, and not disabled; objective items that are not the quest's own start item and that neither
a loot source of the planner (creature or object loot flagged for the quest or from 10 %, one level of reference
loot) nor a vendor gives. The column "how the game hands it out" is what the database says; rows that start with
"made by using an item" or "made by using an object" are the quest interactions of `QuestInteraction`.

| Quest | Level | Title | Item | How the game hands it out |
|---|---|---|---|---|
| 1099 | 1 | Goblins Win! | Goblin Voucher (5769) | nothing found (a C++ script, a talk without SmartAI, or a mail) |
| 921 | 5 | Crown of the Earth | Filled Crystal Phial (5184) | made by using an item: Crystal Phial (5185) = the quest's start item (spell 4976) |
| 929 | 5 | Crown of the Earth | Filled Jade Phial (5639) | made by using an item: Jade Phial (5619) = the quest's start item (spell 6609); made by spell 9582 "Filling" (cast by a script or an NPC) |
| 746 | 8 | Dwarven Digging | Broken Tools (4703) | made by using an item: Prospector's Pick (4702) = handed out with the quest, OLDCeremonial Club (4704), OLDCeremonial Club (5410) (spell 4954) |
| 2459 | 8 | Ferocitas the Dream Eater | Tallonkai's Jewel (8050) | inside the item Gnarlpine Necklace (8049); opened by the planner since the container commit |
| 14436 | 8 | Dwarven Digging | Broken Tools (4703) | made by using an item: Prospector's Pick (4702) = handed out with the quest, OLDCeremonial Club (4704), OLDCeremonial Club (5410) (spell 4954) |
| 812 | 9 | Need for a Cure | Venomtail Antidote (4904) | reward of quest 813 "Finding the Antidote"; the planner takes that quest first |
| 933 | 9 | Crown of the Earth | Filled Tourmaline Phial (5645) | made by using an item: Tourmaline Phial (5621) = the quest's start item (spell 6610) |
| 934 | 11 | Crown of the Earth | Vial of Blessed Water (5646) | made by using an item: Amethyst Phial (5623) = the quest's start item (spell 6611); made by using an item: Eridan's Vial (11682) (spell 15491) |
| 7383 | 11 | Crown of the Earth | Filled Amethyst Phial (18151) | made by using an item: Amethyst Phial (18152) = the quest's start item (spell 22562) |
| 9581 | 11 | Learning from the Crystals | Impact Site Crystal Sample (23878) | made by using an item: Crystal Mining Pick (23875) = the quest's start item (spell 30611) |
| 1141 | 14 | The Family and the Fishing Pole | Darkshore Grouper (12238) | reference loot (low chance, a group, or nested) |
| 4812 | 14 | As Water Cascades | Moonwell Water Tube (14339) | made by using an item: Empty Water Tube (14338) = the quest's start item (spell 18397) |
| 9584 | 14 | The Second Sample | Altered Crystal Sample (23879) | made by using an item: Crystal Mining Pick (23876) = the quest's start item (spell 30612) |
| 116 | 15 | Dry Times | Keg of Thunderbrew Lager (1262) | reward of quest 117 "Thunderbrew Lager"; the planner takes that quest first |
| 4762 | 15 | The Cliffspring River | Cliffspring River Sample (12349) | made by using an item: Empty Sampling Tube (12350) = the quest's start item (spell 16077) |
| 9585 | 18 | The Final Sample | Axxarien Crystal Sample (23880) | made by using an item: Crystal Mining Pick (23877) = the quest's start item (spell 30617) |
| 9748 | 18 | Don't Drink the Water | Bloodmyst Water Sample (24317) | made by using an item: Water Sample Flask (24318) = the quest's start item (spell 31549) |
| 3924 | 19 | Samophlange Manual | Samophlange Manual (11149) | made by using an item: Samophlange Manual Cover (11147) = handed out with the quest (spell 13714); made by using an item: Samophlange Manual Page (11148) = handed out with the quest (spell 14199); made by using an item: DEBUG Samophlange Manual Cover (11613) (spell 15303); made by spell 15304 "DEBUG Create Samophlange Manual" (cast by a script or an NPC) |
| 6543 | 19 | The Warsong Reports | Warsong Scout Update (16764) | reward of quest 6547 "Warsong Scout Update"; the planner takes that quest first |
| 6543 | 19 | The Warsong Reports | Warsong Runner Update (16763) | reward of quest 6545 "Warsong Runner Update"; the planner takes that quest first |
| 6543 | 19 | The Warsong Reports | Warsong Outrider Update (16765) | reward of quest 6546 "Warsong Outrider Update"; the planner takes that quest first |
| 1073 | 21 | Ineptitude + Chemicals = Fun | Elixir of Minor Fortitude (2458) | reward of quest 451 "A Recipe For Death"; the planner takes that quest first; crafted: Alchemy (spell 2334 "Elixir of Minor Fortitude") |
| 1016 | 24 | Elemental Bracers | Divined Scroll (5455) | made by using an item: Divining Scroll (5456) = the quest's start item (spell 6310) |
| 1195 | 25 | The Sacred Flame | Filled Etched Phial (5868) | made by using an item: Etched Phial (5867) = handed out with the quest (spell 6803) |
| 6571 | 27 | Warsong Supplies | Warsong Saw Blades (16742) | reward of quest 6581 "Warsong Saw Blades"; the planner takes that quest first |
| 9433 | 28 | A Dip in the Moonwell | Thalanaar Moonwell Water (23670) | made by spell 29992 "Gather Water" (cast by a script or an NPC) |
| 515 | 30 | Elixir of Agony | Strong Troll's Blood Elixir (3388) | crafted: Alchemy (spell 3176 "Strong Troll's Blood Elixir") |
| 6504 | 30 | The Lost Pages | Shredder Operating Manual - Chapter 1 (16642) | made by using an item: Shredder Operating Manual - Page 1 (16645), Shredder Operating Manual - Page 2 (16646), Shredder Operating Manual - Page 3 (16647) (spell 20529) |
| 6504 | 30 | The Lost Pages | Shredder Operating Manual - Chapter 2 (16643) | made by using an item: Shredder Operating Manual - Page 5 (16649), Shredder Operating Manual - Page 6 (16650), Shredder Operating Manual - Page 7 (16651) (spell 20530) |
| 6504 | 30 | The Lost Pages | Shredder Operating Manual - Chapter 3 (16644) | made by using an item: Shredder Operating Manual - Page 9 (16653), Shredder Operating Manual - Page 10 (16654), Shredder Operating Manual - Page 11 (16655) (spell 20531) |
| 1435 | 33 | The Burning of Spirits | Infused Burning Gem (6435) | nothing found (a C++ script, a talk without SmartAI, or a mail) |
| 565 | 34 | Bartolo's Yeti Fur Cloak | Hillman's Cloak (3719) | crafted: Leatherworking (spell 3760 "Hillman's Cloak") |
| 1371 | 35 | Gizmo for Warug | Advanced Target Dummy (4392) | crafted: Engineering (spell 3965 "Advanced Target Dummy") |
| 2948 | 35 | Gnome Improvement | Silver Bar (2842) | reward of quest 140 "Captain Sanders' Hidden Treasure"; the planner takes that quest first; crafted: Mining (spell 2658 "Smelt Silver"); object loot, best chance 0.0 % (mining veins, herbs, chests) |
| 2950 | 35 | Nogg's Ring Redo | Silver Bar (2842) | reward of quest 140 "Captain Sanders' Hidden Treasure"; the planner takes that quest first; crafted: Mining (spell 2658 "Smelt Silver"); object loot, best chance 0.0 % (mining veins, herbs, chests) |
| 713 | 37 | Coolant Heads Prevail | Frost Oil (3829) | inside the item Cenarion Circle Cache (11887); opened by the planner since the container commit; inside the item Sturdy Junkbox (16884); opened by the planner since the container commit; crafted: Alchemy (spell 3454 "Frost Oil") |
| 715 | 37 | Liquid Stone | Lesser Invisibility Potion (3823) | crafted: Alchemy (spell 3448 "Lesser Invisibility Potion") |
| 5386 | 37 | Catch of the Day | Bloodbelly Fish (13546) | reward of quest 5421 "Fish in a Bucket"; the planner takes that quest first |
| 11140 | 37 | Recover the Cargo! | Salvaged Strongbox (33041) | made by spell 42288 "Summon Salvaged Lockbox" (cast by a script or an NPC) |
| 11140 | 37 | Recover the Cargo! | Salvage Kit (33044) | inside the item Renn's Supplies (33045); opened by the planner since the container commit |
| 11144 | 38 | Confirming the Suspicion | Grimtotem Battle Plan (33051) | made by using an item: Grimtotem Note (33050) = handed out with the quest (spell 42301) |
| 11201 | 38 | The Grimtotem Plot | Grimtotem Battle Plan (33051) | made by using an item: Grimtotem Note (33050) = handed out with the quest (spell 42301) |
| 11161 | 39 | The Essence of Enmity | Black Dragonkin Essence (33087) | made by using an item: Brogg's Totem (33088) = the quest's start item (spell 42411) |
| 338 | 40 | The Green Hills of Stranglethorn | Green Hills of Stranglethorn - Chapter I (2756) | reward of quest 339 "Chapter I"; the planner takes that quest first |
| 338 | 40 | The Green Hills of Stranglethorn | Green Hills of Stranglethorn - Chapter II (2757) | reward of quest 340 "Chapter II"; the planner takes that quest first |
| 338 | 40 | The Green Hills of Stranglethorn | Green Hills of Stranglethorn - Chapter III (2758) | reward of quest 341 "Chapter III"; the planner takes that quest first |
| 338 | 40 | The Green Hills of Stranglethorn | Green Hills of Stranglethorn - Chapter IV (2759) | reward of quest 342 "Chapter IV"; the planner takes that quest first |
| 339 | 40 | Chapter I | Green Hills of Stranglethorn - Page 1 (2725) | creature loot, best chance 2.7 %; reference loot (low chance, a group, or nested) |
| 339 | 40 | Chapter I | Green Hills of Stranglethorn - Page 4 (2728) | creature loot, best chance 1.6 %; reference loot (low chance, a group, or nested) |
| 339 | 40 | Chapter I | Green Hills of Stranglethorn - Page 6 (2730) | creature loot, best chance 4.4 %; reference loot (low chance, a group, or nested) |
| 339 | 40 | Chapter I | Green Hills of Stranglethorn - Page 8 (2732) | creature loot, best chance 8.3 %; reference loot (low chance, a group, or nested) |
| 340 | 40 | Chapter II | Green Hills of Stranglethorn - Page 10 (2734) | creature loot, best chance 1.6 %; reference loot (low chance, a group, or nested) |
| 340 | 40 | Chapter II | Green Hills of Stranglethorn - Page 11 (2735) | creature loot, best chance 4.4 %; reference loot (low chance, a group, or nested) |
| 340 | 40 | Chapter II | Green Hills of Stranglethorn - Page 14 (2738) | creature loot, best chance 1.8 %; reference loot (low chance, a group, or nested) |
| 340 | 40 | Chapter II | Green Hills of Stranglethorn - Page 16 (2740) | creature loot, best chance 6.3 %; reference loot (low chance, a group, or nested) |
| 341 | 40 | Chapter III | Green Hills of Stranglethorn - Page 18 (2742) | creature loot, best chance 6.7 %; reference loot (low chance, a group, or nested) |
| 341 | 40 | Chapter III | Green Hills of Stranglethorn - Page 20 (2744) | creature loot, best chance 4.7 %; reference loot (low chance, a group, or nested) |
| 341 | 40 | Chapter III | Green Hills of Stranglethorn - Page 21 (2745) | creature loot, best chance 1.6 %; reference loot (low chance, a group, or nested) |
| 341 | 40 | Chapter III | Green Hills of Stranglethorn - Page 24 (2748) | creature loot, best chance 2.7 %; reference loot (low chance, a group, or nested) |
| 342 | 40 | Chapter IV | Green Hills of Stranglethorn - Page 25 (2749) | creature loot, best chance 2.3 %; reference loot (low chance, a group, or nested) |
| 342 | 40 | Chapter IV | Green Hills of Stranglethorn - Page 26 (2750) | creature loot, best chance 3.2 %; reference loot (low chance, a group, or nested) |
| 342 | 40 | Chapter IV | Green Hills of Stranglethorn - Page 27 (2751) | creature loot, best chance 9.3 %; reference loot (low chance, a group, or nested) |
| 716 | 42 | Stone Is Better than Cloth | Patterned Bronze Bracers (2868) | crafted: Blacksmithing (spell 2672 "Patterned Bronze Bracers") |
| 50 | 44 | Sweet Amber | Truesilver Bar (6037) | crafted: Mining (spell 10098 "Smelt Truesilver"); crafted: Alchemy (spell 11480 "Transmute: Mithril to Truesilver"); object loot, best chance 8.6 % (mining veins, herbs, chests) |
| 2203 | 44 | Badlands Reagent Run II | Vessel of Dragon's Blood (7867) | made by spell 21029 "Thaumaturgy Channel" (cast by a script or an NPC) |
| 2501 | 44 | Badlands Reagent Run II | Vessel of Dragon's Blood (7867) | made by spell 21029 "Thaumaturgy Channel" (cast by a script or an NPC) |
| 3125 | 45 | Faerie Dragon Muisek | Faerie Dragon Muisek (9596) | made by using an item: Faerie Dragon Muisek Vessel (9620) = the quest's start item (spell 11888) |
| 592 | 46 | Saving Yenniku | Filled Soul Gem (3913) | reward of quest 593 "Filling the Soul Gem"; the planner takes that quest first |
| 992 | 46 | Gadgetzan Water Survey | Tapped Dowsing Widget (8585) | made by using an item: Untapped Dowsing Widget (8584) = the quest's start item (spell 11758) |
| 3123 | 47 | Testing the Vessel | Beast Muisek (9594) | made by using an item: Beast Muisek Vessel (9618) = the quest's start item (spell 11886) |
| 3124 | 47 | Hippogryph Muisek | Hippogryph Muisek (9595) | made by using an item: Hippogryph Muisek Vessel (9619) = the quest's start item (spell 11887) |
| 3842 | 47 | A Short Incubation | Elixir of Fortitude (3825) | reward of quest 1222 "Stinky's Escape"; the planner takes that quest first; reward of quest 1270 "Stinky's Escape"; the planner takes that quest first; crafted: Alchemy (spell 3450 "Elixir of Fortitude") |
| 2944 | 48 | The Super Snapper FX | Snapshot of Gammerita (9330) | made by using an item: Super Snapper FX (9328) = the quest's start item (spell 11610) |
| 3381 | 48 | A Meeting with the Master | Undamaged Hippogryph Feather (10450) | nothing found (a C++ script, a talk without SmartAI, or a mail) |
| 2879 | 50 | The Stave of Equinex | Stave of Equinex (9306) | made by using an item: Troyas' Stave (9263) = the quest's start item (spell 11537) |
| 2942 | 50 | The Morrow Stone | A Sparkling Stone (9307) | reward of quest 2879 "The Stave of Equinex"; the planner takes that quest first; made by spell 15187 "Create Sparkling Stone" (cast by a script or an NPC) |
| 3126 | 50 | Treant Muisek | Treant Muisek (9593) | made by spell 11884 "Create Treant Muisak" (cast by a script or an NPC); made by using an item: Treant Muisek Vessel (9606) = the quest's start item (spell 11885) |
| 3786 | 50 | Morrowgrain Research | Morrowgrain (11040) | inside the item Evergreen Herb Casing (11024); opened by the planner since the container commit |
| 3791 | 50 | The Mystery of Morrowgrain | Morrowgrain (11040) | inside the item Evergreen Herb Casing (11024); opened by the planner since the container commit |
| 4296 | 50 | Tablet of the Seven | Tablet Transcript (11470) | made by spell 15065 "Create Tablet Transcript" (cast by a script or an NPC) |
| 3568 | 52 | Seeping Corruption | Filled Vial Labeled #1 (10691) | made by using an item: Empty Vial Labeled #1 (10687) (spell 12802) |
| 3568 | 52 | Seeping Corruption | Filled Vial Labeled #2 (10692) | made by using an item: Empty Vial Labeled #2 (10688) (spell 12805) |
| 3568 | 52 | Seeping Corruption | Filled Vial Labeled #3 (10693) | made by using an item: Empty Vial Labeled #3 (10689) (spell 12806) |
| 3568 | 52 | Seeping Corruption | Filled Vial Labeled #4 (10694) | made by using an item: Empty Vial Labeled #4 (10690) (spell 12808) |
| 3845 | 52 | It's a Secret to Everybody | Large Compass (11104) | inside the item A Small Pack (11107); opened by the planner since the container commit; fishing |
| 3845 | 52 | It's a Secret to Everybody | Curled Map Parchment (11105) | inside the item A Small Pack (11107); opened by the planner since the container commit; fishing |
| 3845 | 52 | It's a Secret to Everybody | Lion-headed Key (11106) | inside the item A Small Pack (11107); opened by the planner since the container commit; fishing |
| 3883 | 52 | Alien Ecology | Hive Wall Sample (11131) | made by using an item: Unused Scraping Vial (11132) = the quest's start item (spell 13630) |
| 3909 | 52 | The Videre Elixir | Videre Elixir (11243) | reward of quest 4041 "The Videre Elixir"; the planner takes that quest first |
| 4293 | 52 | A Sample of Slime... | Corrupted Felwood Sample (12234) | inside the item Corrupt Tested Sample (15103); opened by the planner since the container commit |
| 4512 | 52 | A Little Slime Goes a Long Way | Filled Cursed Ooze Jar (11947) | made by using an item: Empty Cursed Ooze Jar (11914) (spell 15698) |
| 4512 | 52 | A Little Slime Goes a Long Way | Filled Tainted Ooze Jar (11949) | made by using an item: Empty Tainted Ooze Jar (11948) (spell 15699) |
| 5157 | 52 | Collection of the Corrupt Water | Corrupt Moonwell Water (12907) | made by using an item: Empty Canteen (12922) = the quest's start item (spell 17161) |
| 13906 | 53 | They Grow Up So Fast | Venomhide Baby Tooth (47196) | reward of quest 13889 "Hungry, Hungry Hatchling"; the planner takes that quest first; reward of quest 13903 "Gorishi Grub"; the planner takes that quest first; reward of quest 13904 "Poached, Scrambled, Or Raw?"; the planner takes that quest first; reward of quest 13905 "Searing Roc Feathers"; the planner takes that quest first; reward of quest 13914 "Searing Roc Feathers"; the planner takes that quest first; reward of quest 13915 "Hungry, Hungry Hatchling"; the planner takes that quest first; reward of quest 13916 "Poached, Scrambled, Or Raw?"; the planner takes that quest first; reward of quest 13917 "Gorishi Grub"; the planner takes that quest first |
| 4005 | 54 | Aquementas | Silver Totem of Aquementas (11522) | nothing found (a C++ script, a talk without SmartAI, or a mail) |
| 4441 | 54 | Felbound Ancients | Vial of Blessed Water (5646) | made by using an item: Amethyst Phial (5623) (spell 6611); made by using an item: Eridan's Vial (11682) = the quest's start item (spell 15491) |
| 4505 | 54 | Well of Corruption | Filled Flasket (12567) | made by using an item: Hardened Flasket (12566) = the quest's start item (spell 15649) |
| 4513 | 54 | A Little Slime Goes a Long Way | Filled Pure Sample Jar (11954) | made by using an item: Empty Pure Sample Jar (11953) (spell 15702) |
| 5051 | 54 | Two Halves Become One | Good Luck Charm (12723) | made by using an item: Good Luck Other-Half-Charm (12722) = handed out with the quest (spell 16781) |
| 3374 | 55 | The Essence of Eranikus | Chained Essence of Eranikus (10455) | reward of quest 3373 "The Essence of Eranikus"; the planner takes that quest first |
| 3501 | 55 | Everything Counts In Large Amounts | Imperfect Draenethyst Fragment (10593) | creature loot, best chance 3.0 % |
| 4106 | 55 | Salve via Skinning | Patch of Tainted Skin (11512) | skinning |
| 4107 | 55 | Salve via Disenchanting | Lesser Nether Essence (11174) | made by using an item: Greater Nether Essence (11175) (spell 13740) |
| 5149 | 55 | Pamela's Doll | Pamela's Doll (12885) | made by using an item: Pamela's Doll's Head (12886) = handed out with the quest, Pamela's Doll's Left Side (12887) = handed out with the quest, Pamela's Doll's Right Side (12888) = handed out with the quest (spell 17133) |
| 5885 | 55 | Salve via Skinning | Patch of Tainted Skin (11512) | skinning |
| 5886 | 55 | Salve via Disenchanting | Lesser Nether Essence (11174) | made by using an item: Greater Nether Essence (11175) (spell 13740) |
| 6032 | 55 | Sacred Cloth | Mooncloth (14342) | crafted: Tailoring (spell 18560 "Mooncloth") |
| 4294 | 56 | ... and a Batch of Ooze | Pure Un'Goro Sample (12236) | inside the item Un'Goro Tested Sample (15102); opened by the planner since the container commit |
| 5247 | 57 | Fragments of the Past | Enchanted Thorium Bar (12655) | crafted: Enchanting (spell 17180 "Enchanted Thorium"); crafted: Mining (spell 70524 "Enchanted Thorium") |
| 5247 | 57 | Fragments of the Past | Crystal Restore (11562) | reward of quest 4381 "Crystal Restore"; the planner takes that quest first |
| 5247 | 57 | Fragments of the Past | Vial of Dire Water (16973) | made by using an item: Empty Water Vial (16974) = the quest's start item (spell 20814) |
| 5801 | 57 | Fire Plume Forged | Unfinished Skeleton Key (14645) | made by using an item: Skeleton Key Mold (14644) = the quest's start item (spell 18711) |
| 5802 | 57 | Fire Plume Forged | Unfinished Skeleton Key (14645) | made by using an item: Skeleton Key Mold (14644) = the quest's start item (spell 18711) |
| 6022 | 58 | To Kill With Purpose | Coagulated Rot (15448) | made by using an item: Mortar and Pestle (15454) = the quest's start item (spell 19029) |
| 6026 | 58 | That's Asking A Lot | Thorium Bar (12359) | crafted: Mining (spell 16153 "Smelt Thorium") |
| 6026 | 58 | That's Asking A Lot | Golden Rod (11128) | crafted: Blacksmithing (spell 14379 "Golden Rod") |
| 6026 | 58 | That's Asking A Lot | Hi-Explosive Bomb (10562) | inside the item Goblin Engineer's Renewal Gift (11422); opened by the planner since the container commit; crafted: Engineering (spell 12619 "Hi-Explosive Bomb") |
| 6026 | 58 | That's Asking A Lot | Unstable Trigger (10560) | inside the item Gnome Engineer's Renewal Gift (11423); opened by the planner since the container commit; crafted: Engineering (spell 12591 "Unstable Trigger") |

## Counts by first way

- 47 x made by using an item
- 10 x crafted
- 4 x nothing found
- 3 x creature loot, best chance 1.6 %
- 3 x inside the item A Small Pack
- 2 x reward of quest 140 "Captain Sanders' Hi
- 2 x creature loot, best chance 2.7 %
- 2 x creature loot, best chance 4.4 %
- 2 x made by spell 21029 "Thaumaturgy Channel
- 2 x inside the item Evergreen Herb Casing
- 2 x skinning
- 1 x inside the item Gnarlpine Necklace
- 1 x reward of quest 813 "Finding the Antidot
- 1 x reference loot
- 1 x reward of quest 117 "Thunderbrew Lager"
- 1 x reward of quest 6547 "Warsong Scout Upda
- 1 x reward of quest 6545 "Warsong Runner Upd
- 1 x reward of quest 6546 "Warsong Outrider U
- 1 x reward of quest 451 "A Recipe For Death"
- 1 x reward of quest 6581 "Warsong Saw Blades
- 1 x made by spell 29992 "Gather Water"
- 1 x inside the item Cenarion Circle Cache
- 1 x reward of quest 5421 "Fish in a Bucket"
- 1 x made by spell 42288 "Summon Salvaged Loc
- 1 x inside the item Renn's Supplies
- 1 x reward of quest 339 "Chapter I"
- 1 x reward of quest 340 "Chapter II"
- 1 x reward of quest 341 "Chapter III"
- 1 x reward of quest 342 "Chapter IV"
- 1 x creature loot, best chance 8.3 %
- 1 x creature loot, best chance 1.8 %
- 1 x creature loot, best chance 6.3 %
- 1 x creature loot, best chance 6.7 %
- 1 x creature loot, best chance 4.7 %
- 1 x creature loot, best chance 2.3 %
- 1 x creature loot, best chance 3.2 %
- 1 x creature loot, best chance 9.3 %
- 1 x reward of quest 593 "Filling the Soul Ge
- 1 x reward of quest 1222 "Stinky's Escape"
- 1 x reward of quest 2879 "The Stave of Equin
- 1 x made by spell 11884 "Create Treant Muisa
- 1 x made by spell 15065 "Create Tablet Trans
- 1 x reward of quest 4041 "The Videre Elixir"
- 1 x inside the item Corrupt Tested Sample
- 1 x reward of quest 13889 "Hungry, Hungry Ha
- 1 x reward of quest 3373 "The Essence of Era
- 1 x creature loot, best chance 3.0 %
- 1 x inside the item Un'Goro Tested Sample
- 1 x reward of quest 4381 "Crystal Restore"
- 1 x inside the item Goblin Engineer's Renewa
- 1 x inside the item Gnome Engineer's Renewal
