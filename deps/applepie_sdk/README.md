# Bundled Applepie SDK

This directory contains the unmodified public headers needed to build EndfieldCombatHUD, including all transitive SDK header dependencies of `applepie_client.h`.

- Upstream: [Sasye/ApplepieManager](https://github.com/Sasye/ApplepieManager).
- Source commit: [`e5facbf7f3716f8abff2025061819983fcebb0f3`](https://github.com/Sasye/ApplepieManager/tree/e5facbf7f3716f8abff2025061819983fcebb0f3/sdk).
- SDK ABI: 1.1, including the Config service with completion notifications.
- Included headers: `applepie.h`, `applepie_client.h`, `applepie_native.h`, `applepie_seh.h`, and `applepie_pointer_scope.h`.
- License: the upstream [GNU Affero General Public License v3](LICENSE), copied from the same repository.

The plugin build and release CI consume these headers directly. No Manager source checkout, client library, or SDK download is required. Applepie Manager still provides the runtime services when the plugin is loaded.

To update this snapshot, copy the required headers together from one upstream revision, preserve the upstream license, update this source commit, and rebuild the plugin. Include any new transitive SDK headers introduced by that revision. Keep local plugin adaptations in `src/` rather than changing the public SDK headers.
