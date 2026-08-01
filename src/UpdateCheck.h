#pragma once

#include <functional>
#include <string>

namespace odm {
namespace updates {

/// The page a user is sent to when they act on the notice. Hard-coded on
/// purpose: nothing the network answers ever reaches ShellExecute, so a
/// compromised or spoofed API response cannot turn the notice into a way to
/// launch something.
extern const char* const kReleasesUrl;

/// Is `tag` a release newer than `current`?
///
/// `tag` is a GitHub tag ("v0.2.4"), `current` is ODM_VERSION ("0.2.3"); the
/// leading 'v' is optional on both. Compares the numeric components, so a
/// build running ahead of the newest release (0.3.0 against v0.2.4) is not
/// told to downgrade, and a re-tag with more components (v0.2.4.1) still
/// registers. Anything unparsable answers false: a notice we cannot justify
/// is worse than no notice.
bool IsNewerVersion(const std::string& tag, const std::string& current);

/// Pull the tag name out of a GitHub "releases/latest" response.
/// Returns false when the body isn't what we expect.
bool ParseLatestTag(const std::string& json, std::string* out_tag);

/// Ask GitHub whether a newer release exists, off-thread, and call `on_newer`
/// with the bare version ("0.2.4") if one does.
///
/// Silent about everything else: no network, a rate-limited API, a malformed
/// answer and "already up to date" all look the same from outside, because
/// none of them is something to interrupt someone's downloads over. Checked
/// at most once a day — the stamp file next to the executable is written
/// before the request, so a machine that cannot reach GitHub does not retry
/// on every launch.
///
/// `on_newer` runs on the worker thread; marshal to the UI yourself.
void CheckAsync(std::function<void(const std::string& version)> on_newer);

}  // namespace updates
}  // namespace odm
