# The per-user half of the installer (installer\bada.iss runs it as the user who started Setup,
# not elevated): adds bada to the Japanese language's keyboards, or removes it. Other input
# methods and the rest of the language list are left alone.
#   keyboards.ps1           add bada (and Japanese, if it isn't in the list)
#   keyboards.ps1 -Remove   take it out again
param([switch] $Remove)
$ErrorActionPreference = 'Stop'
# The language (ja-JP), the text service's class and its profile (src\tip\guids.h).
$tip = '0411:{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}{89CC34D8-9E73-44BD-B1F9-301D19A5C30F}'

$list = Get-WinUserLanguageList
$japanese = $null
foreach ($l in $list.ToArray()) { if ($l.LanguageTag -eq 'ja') { $japanese = $l } }
if ($Remove) {
    if ($japanese -and $japanese.InputMethodTips -contains $tip) {
        [void]$japanese.InputMethodTips.Remove($tip)
        Set-WinUserLanguageList $list -Force
    }
    exit 0
}
if (-not $japanese) {
    # Japanese wasn't one of the user's languages: it is added with bada as its only keyboard.
    $japanese = (New-WinUserLanguageList 'ja')[0]
    $japanese.InputMethodTips.Clear()
    $list.Add($japanese)
}
if ($japanese.InputMethodTips -notcontains $tip) {
    $japanese.InputMethodTips.Add($tip)
    Set-WinUserLanguageList $list -Force
}
exit 0
