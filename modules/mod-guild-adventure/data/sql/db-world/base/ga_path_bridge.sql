-- mod-guild-adventure: crossings of gaps in the path finder's ground (GA_world).
-- Applied by the worldserver's DB updater; every statement is idempotent. Design: docs/orders.md "Live round 42"
-- (round 80), src/OrderRoutes.cpp (OrderService::BridgeGap).

-- The navmesh has places where two pieces of walkable ground do not connect although characters walk from one to
-- the other (a corridor's junction, a doorway): the path finder ends at the gap. One row per such place: two points,
-- one on each piece, a few yards apart with open floor between them. A bot whose path ends short of its target
-- walks to the one point and steps straight to the other. Hand-checked rows only (two points a patrol walks between,
-- or positions read in the client with .gps).
CREATE TABLE IF NOT EXISTS `ga_path_bridge` (
    `id`      INT UNSIGNED NOT NULL,
    `map`     SMALLINT UNSIGNED NOT NULL,
    `x1`      FLOAT NOT NULL,
    `y1`      FLOAT NOT NULL,
    `z1`      FLOAT NOT NULL,
    `x2`      FLOAT NOT NULL,
    `y2`      FLOAT NOT NULL,
    `z2`      FLOAT NOT NULL,
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild Adventure: crossings of gaps in the path finder''s ground';

DELETE FROM `ga_path_bridge` WHERE `id` IN (1);
INSERT INTO `ga_path_bridge` (`id`, `map`, `x1`, `y1`, `z1`, `x2`, `y2`, `z2`, `comment`) VALUES
-- Stormwind Keep: where the corridor to the Royal Library leaves the entrance ramp. The hall's ground ends at the
-- first point, the library wing's begins at the second (probed with .ga admin path); a guard's patrol walks across
-- (waypoint_data 105280, points 8 to 9). Behind it: Milton Sheaf ("An Old History Book"), Donyal Tovald.
(1, 0, -8471.8, 413.0, 111.6, -8469.3, 413.0, 112.0, 'Stormwind Keep: the corridor to the Royal Library');
