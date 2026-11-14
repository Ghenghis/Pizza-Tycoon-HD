# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PSScriptAnalyzer rules for scripts\*.ps1 (run by scripts\lint.ps1).
# Every Warning and Error rule applies, except those listed with a reason.
@{
    Severity     = @('Error', 'Warning')
    ExcludeRules = @(
        # These are console scripts: the coloured PASS/FAIL/SKIP lines are
        # meant for the person running them, not for the pipeline.
        'PSAvoidUsingWriteHost'
    )
}
