#!/bin/sh
# Regenerate generated/ from config/pz_config.toml. Log -> docs/recomp_default.log
cd "$(dirname "$0")/.." || exit 1
# Manual jump tables survive a XenonAnalyse rerun: drop the old copy, append the current one.
sed -i '/^# ---- MANUAL (config\/pz_switches_manual.toml) ----$/,$d' config/pz_switches.toml
cat config/pz_switches_manual.toml >> config/pz_switches.toml
# XenonRecomp never deletes stale ppc_recomp.N.cpp files (the build globs them): start clean.
rm -rf generated && mkdir generated
external/XenonRecomp/build/XenonRecomp/XenonRecomp.exe config/pz_config.toml external/XenonRecomp/XenonUtils/ppc_context.h > docs/recomp_default.log 2>&1
rc=$?
L=docs/recomp_default.log
echo "rc=$rc unrec=$(grep -c Unrecognized $L) switcherr=$(grep -c 'Switch case' $L) badsw=$(grep 'Switch case' $L | awk '{print $5}' | sort -u | wc -l) funcs=$(grep -c ', sub_' generated/ppc_func_mapping.cpp)"
grep -v 'Unrecognized\|Switch case\|^Recompiling' $L | sort | uniq -c | sort -rn | head -5
