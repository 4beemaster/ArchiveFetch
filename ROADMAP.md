# Planned Updates

Working plan; versions and details can still change.

## Released

- **0.3.0**: source collections, Clean library, a password for outside devices, scanning while downloading.
- **0.4.0**: archive extraction on the PS5 (ZIP, 7z, RAR, tar) with a global setting, a per-location setting and an option to delete the archive afterwards; progress for joining, verifying and extracting.
- **0.5.0**: queueing while a download runs, an Extension column and filter, configurable hidden file types (the catalog keeps every type), click-to-sort column headers.
- **0.6.0**: cleanup with no new features: one version number for the Windows, Android and PS5 apps, the old SDL desktop app retired, the large shared files split; fixes for Android Settings saving and for clean Windows builds.
- **0.7.0**: the Windows and Android apps connect to consoles: saved consoles with status dots and a target switcher, remote control through the app's backend, password sign-in and permanent pairing, a home-network scan, Install / Update of the console's ArchiveFetch from the app, and an add-only Merge in either direction; also comma-separated hidden file types and a hide button with Undo.
- **0.7.1**: fixes: Clean library with hidden file types, deleting extracted folders on the PS5 (with the real space freed), extracted downloads recognized in every download location, a simpler Clean library popup (delete downloads and/or remove entries, with a second confirmation), and no local build paths in the Android app.
- **0.7.2**: console updates go through ArchiveFetch on port 8890, which hands the payload to the console's own loader, so the loader port no longer has to be reachable from other devices; a banner shows when a console runs an older or newer ArchiveFetch than the app; Downloads shows each transfer's speed and time left; **Connections per download** (1 to 8, default 4, now also on Windows and Android) with refused parts retrying on their own; request bodies other than updates are limited to 1 MB.
- **0.7.3**: app-driven console updates also replace the existing ArchiveFetch copy through itsPLK Payload Manager's own API, preserving its filename, slot and boot list. Compatible with Payload Manager 0.6.3's upload guard. Failed synchronization keeps the running update and shows a warning with Retry update. Windows gains a per-user installer with data-preserving manual upgrades.

## Next

- **0.8.0-beta.4** (acceptance testing): adds individual-download Pause/Resume while retaining partial data and continuing the rest of the queue. Explicitly paused files wait for Resume; Stop/Start retains its queue-wide behavior. Extraction is not pausable. Includes beta.2's verified GitHub updater, tested end-to-end through beta.3; beta.4 receives another Windows, Android-emulator and real-PS5 upgrade/pause regression cycle before review. No automatic background checks or unattended installation. PS5 updates preserve the existing Payload Manager filename, slot and boot list. Development stops for review at beta.4. Android release signing remains separate future work.

## 1.0.0

- The PS5, Windows and Android versions feature-matched and in use for a while.

## Later

- Drive and network-share folders as sources (local/USB drives first; SMB, FTP and NFS once there is a share to test against).
- List an archive's contents before downloading it.
- Archive extraction in the Windows and Android apps.
