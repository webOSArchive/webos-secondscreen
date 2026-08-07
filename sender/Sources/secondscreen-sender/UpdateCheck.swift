/* Startup update check against GitHub releases. Best-effort and
 * notify-only: on a newer release tag we log it and light up a menu-bar
 * item linking to the release page — no self-updating.
 *
 * The receiver has its own update path (App Museum II); versions across
 * the two are kept in sync by hand. */
import Foundation

// Single source of truth for the sender version — package-app.sh reads
// this line for the bundle's Info.plist. Keep equal to the GitHub
// release tag.
let appVersion = "0.2.5"

private let latestReleaseAPI =
    "https://api.github.com/repos/webOSArchive/webos-secondscreen/releases/latest"
private let releasesPage =
    "https://github.com/webOSArchive/webos-secondscreen/releases"

/// "v0.2.3" / "0.2.3" → [0, 2, 3]; unparsable → []
private func parseVersion(_ s: String) -> [Int] {
    let t = s.trimmingCharacters(in: .whitespaces)
    let core = t.hasPrefix("v") ? t.dropFirst() : t[...]
    return core.split(separator: ".").compactMap { Int($0) }
}

private func isNewer(_ remote: [Int], than local: [Int]) -> Bool {
    for i in 0..<max(remote.count, local.count) {
        let r = i < remote.count ? remote[i] : 0
        let l = i < local.count ? local[i] : 0
        if r != l { return r > l }
    }
    return false
}

/// Async; never blocks startup, never fatal.
func checkForUpdate() {
    guard let url = URL(string: latestReleaseAPI) else { return }
    var req = URLRequest(url: url, timeoutInterval: 10)
    req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
    req.setValue("webos-secondscreen-sender/\(appVersion)", forHTTPHeaderField: "User-Agent")
    URLSession.shared.dataTask(with: req) { data, resp, _ in
        guard let http = resp as? HTTPURLResponse else {
            log("update check: no response (offline?); skipping")
            return
        }
        guard http.statusCode == 200, let data,
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tag = obj["tag_name"] as? String else {
            // 404 = repo has no releases yet; 403 = API rate limit
            log("update check: unavailable (HTTP \(http.statusCode)); skipping")
            return
        }
        let remote = parseVersion(tag)
        guard !remote.isEmpty else {
            log("update check: unparsable release tag '\(tag)'; skipping")
            return
        }
        if isNewer(remote, than: parseVersion(appVersion)) {
            let page = (obj["html_url"] as? String).flatMap(URL.init(string:))
                ?? URL(string: releasesPage)!
            log("update available: \(tag) (running \(appVersion)) — \(page.absoluteString)")
            StatusUI.shared.showUpdateAvailable(version: tag, url: page)
        } else {
            log("up to date (\(appVersion); latest release \(tag))")
        }
    }.resume()
}
