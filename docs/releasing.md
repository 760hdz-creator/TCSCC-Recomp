# Releasing a new version (in-game updater feed)

The game's updater (`runtime/src/updater.cpp`) asks
`https://gitlab.com/api/v4/projects/octagram-labs-group%2FTCSCC-Recomp/releases/permalink/latest`
and offers the release only when its tag is a strictly higher version than the running build's
`project(VERSION)`. A release with an unchanged number is never offered to anyone.

1. Bump `project(TCSCCRecomp VERSION x.y.z ...)` in `runtime/CMakeLists.txt`. Commit.
2. `tools\build_runtime.bat`, run `runtime\build\scda_tests.exe`, then the outer
   `tools\installer\build.bat` (it reads the same number; installer shows `x.y.z+<hash>`).
3. Zip: `Compress-Archive TCSCC_Setup.exe "G:\Project Zero\dist\TCSCC_Setup_x.y.z.zip"`.
4. Before pushing: audit tree and history for game files (`*.xex *.img *.iso assets/ generated/`).
   Push `main`, tag `vx.y.z`, and create the release with two links. The exe link must be named
   exactly `TCSCC_Setup.exe`, because the updater picks that name.
   `glab release create vx.y.z "path\TCSCC_Setup.exe#TCSCC_Setup.exe" "path\TCSCC_Setup_x.y.z.zip#TCSCC_Setup_x.y.z.zip" -n "TCSCC Recomp x.y.z" -R octagram-labs-group/TCSCC-Recomp`
5. Check the feed anonymously: `curl -s .../releases/permalink/latest` returns the tag and both
   links; the exe link starts with `MZ` and has the local size.

Test an update without a new release: `SCDA_VERSION_OVERRIDE=0.0.1` (pretend to be older);
`[Update] Repo=group/project` in a `SCDA_SETTINGS` ini points the check at another project.
Scripted runs (`SCDA_AUTOPRESS`) skip the check unless `SCDA_UPDATE_CHECK=1`.
Installs older than 0.1.0 have no working updater: they must install 0.1.0 by hand once.
