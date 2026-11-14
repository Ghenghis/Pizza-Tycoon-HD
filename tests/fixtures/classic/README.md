# Classic golden fingerprints

`golden-fingerprints.json` holds the expected result of every checkpoint in
`tests/ClassicFingerprintTests.cpp` (tag `[golden]`), per platform
(`<os>-<compiler>-<arch>`, for example `windows-msvc-x64`, `linux-gcc-x64`). Each
value is the `SaveGameManager::fingerprint` hash of the game (the whole save
document except the time it was written, random state included) plus the random
state, date, time blocks, restaurant count and number of cities built, which
make a difference easier to read. Only hashes and counts are stored, never the
document, because it holds data from the original game.

The file ships with no values: they need the original game, so a missing value
fails the test instead of being filled in quietly.

Recording (only on a build whose Classic behaviour is the reference, so for the
first time on the commit before the roster refactor):

    PLEGACY_TEST_GAME_PATH=/path/to/PIZZA PLEGACY_UPDATE_GOLDEN=1 \
        test_plegacy "[golden]"

Then review the diff of this file and commit it. Every later run without
`PLEGACY_UPDATE_GOLDEN` compares and fails on any difference, writing the
actual document to the temp folder (`plegacy-golden-<checkpoint>.json`) for
inspection. Re-record only for an intended Classic change, and say so in the
commit message.

Without `PLEGACY_TEST_GAME_PATH` the tests SKIP.
