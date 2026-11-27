# Getting started

## What you need

Your own World of Warcraft 3.3.5a client (build 12340). Guild Adventure contains no game files; the server data is made
from your client.

- **Playing alone on your PC:** use the one-click installer of the repository `guild-adventure-local`. It sets up the
  server, the database, your account and the addon, and gives you `Start.cmd` and `Stop.cmd`.
- **Playing on a server someone runs:** ask them for the realmlist and an account, and copy the addon
  `addon/GuildAdventure` into your client's `Interface\AddOns` folder.
- **Running a server yourself:** [docs/server-setup.md](../docs/server-setup.md).

Make sure the addon **GuildAdventure** is enabled on the character screen (AddOns button, bottom left).

## Your first login

1. Create a character. Any race and class will do; it is a normal character that you level yourself (or let level on
   its own, see [Orders](orders.md#your-leader)).
2. Enter the world. The first character of your account that enters the world becomes your **guild leader**: a guild
   called "*Name* Adventurers" is founded with it as guild master. Your guild's bots belong to the same faction.
3. Open the management window with `/ga` or the minimap button.

The window starts on the **Overview** tab: your guild, its renown, its bot slots, the offline window, the guild bank and
what happened lately.

## Your first bots

The guild starts with three bot slots.

1. Open the **Recruitment** tab.
2. Choose race, class, gender and a name. The races and classes are those of your faction. A Death Knight can be
   recruited once a member of your guild (you or a bot) has reached level 55.
3. Press **Recruit**. The bot is created and enters the world at once.

A new bot starts at level 1 in the starting area of its race (a Death Knight at 55 in its faction's capital) with the
order **Autopilot**: from now on it levels on its own. Recruit the other two the same way.

Bots are real characters and real members of your guild: you see them in the guild roster, they talk in guild chat
(at most one line every half minute or so reaches you), and they can trade with you. They do not show on your
character screen; they live on accounts of their own that the server manages.

## What happens next

- Your bots log in when you log in, and keep playing for a while after you log out (the **offline window**, one hour
  at the start).
- The **Roster** tab lists them with their level, gear level, role, professions, zone, order and state. Click a row for
  the bot's details; **Play** takes it over.
- Each bot shows a "why" line that says what it is doing and why ("Questing in Elwynn Forest: 6 quests left").
- When you log in after they played without you, a "While you were away" window tells you what happened.

Next: [The window](the-window.md).
