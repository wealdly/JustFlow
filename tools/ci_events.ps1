# Why was a binary blocked? Reads the last CodeIntegrity events (Smart App Control / WDAC).
Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 6 -ErrorAction SilentlyContinue |
    ForEach-Object {
        $first = ($_.Message -split "`r?`n")[0]
        "{0}  id={1}  {2}" -f $_.TimeCreated, $_.Id, $first
    }
"--- policy ---"
"SmartAppControl(VerifiedAndReputablePolicyState) = " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name VerifiedAndReputablePolicyState -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
