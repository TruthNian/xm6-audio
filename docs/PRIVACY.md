# Privacy policy

The XM6 Audio drivers and codec service exchange audio and control data with the paired Bluetooth device selected by the user. They do not automatically contact Internet servers, analytics services or a licensing server.

The service reads Windows device information, including the Bluetooth device address, to associate the selected headset with its audio endpoint. Per-device settings and live status use `HKLM\SOFTWARE\OpenA2DP`; local diagnostic logs use `%ProgramData%\OpenA2DP`. Deployment writes a local recovery record under `%ProgramData%\XM6Audio`. Diagnostic data can contain device names and addresses and should be reviewed before sharing.

File decoding remains with the selected media player and Windows. The native path receives PCM from its own audio endpoint; it does not inspect the user's music library or collect other applications' files. Pairing remains managed by Windows.

Nothing is uploaded by deployment or playback. A maintainer may explicitly publish public source/build details in a signing inquiry or attach an artifact to an approved signing request. Such actions require separate authorization. GitHub and Windows operate under their own policies; this policy describes this project's code.
