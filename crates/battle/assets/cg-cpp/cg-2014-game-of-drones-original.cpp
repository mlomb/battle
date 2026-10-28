// Game of Drones, the referee as published (CodinGame, 2014) - headless C++ referee, port of "Referees/Game of
// Drones/Referee.java" of https://github.com/SpiritusSancti5/codinGame (commit c0cc6fe) with only the seed change of
// the patch (one "seed=N" init line seeds the game): the game of cg-2014-game-of-drones.cpp with the file's own zone rule
// (each zone redrawn until it is 700 from the zones before it, without end: a referee that loops forever, i.e. a failed
// game, when no room is left) instead of CodinGame's restart after 501 failed draws. See cg-2014-game-of-drones.cpp for
// everything else (the jar: referees/cg-jar/cg-2014-game-of-drones-original.jar, built with
// referees/cg-builder/patches/cg-2014-game-of-drones-original.patch).
#define GAME_OF_DRONES_ORIGINAL 1
#include "cg-2014-game-of-drones.cpp"
