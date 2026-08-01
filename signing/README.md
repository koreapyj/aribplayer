# Pinned sideload signing certificate

This directory holds the signing identity for ARIB Player sideloaded builds. Debug and release APKs use the same pinned certificate so a user can update an existing sideloaded installation without uninstalling it.

- Alias: `aribplayer`
- SHA-256 certificate fingerprint: `33:75:98:75:D3:D1:27:3C:16:86:7D:AE:D0:0A:83:D4:95:63:5C:97:0F:2C:7F:0F:4E:64:D3:3F:25:A4:D0:A6`

Do not regenerate or lose `aribplayer-upload.jks`: Android will treat a replacement certificate as a different signer, and existing users will have to uninstall and reinstall. The keystore and `keystore.properties` are gitignored. Back them up privately and securely; do not commit the passwords or keystore.
