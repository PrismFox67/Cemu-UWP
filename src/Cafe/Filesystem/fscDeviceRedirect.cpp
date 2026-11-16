#include "util/helpers/helpers.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"
#include "FST/fstUtil.h"

struct RedirectEntry
{
	RedirectEntry(const fs::path& dstPath, sint32 priority) : dstPath(dstPath), priority(priority) {}
	fs::path dstPath;
	sint32 priority;
};

FSAFileTree<RedirectEntry> redirectTree;
std::vector<std::string> redirectPaths; // every path added, so fscDeviceRedirect_reset can remove them

void fscDeviceRedirect_add(std::string_view virtualSourcePath, size_t fileSize, const fs::path& targetFilePath, sint32 priority)
{
	// check if source already has a redirection
	RedirectEntry* existingEntry;
	if (redirectTree.getFile(virtualSourcePath, existingEntry))
	{
		if (existingEntry->priority >= priority)
			return; // dont replace entries with equal or higher priority
		// unregister existing entry
		redirectTree.removeFile(virtualSourcePath);
		delete existingEntry;
	}
	RedirectEntry* entry = new RedirectEntry(targetFilePath, priority);
	redirectTree.addFile(virtualSourcePath, fileSize, entry);
	redirectPaths.emplace_back(virtualSourcePath);
}

class fscDeviceTypeRedirect : public fscDeviceC
{
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		RedirectEntry* redirectionEntry;

		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) && redirectTree.getFile(path, redirectionEntry))
			return FSCVirtualFile_Host::OpenFile(redirectionEntry->dstPath, accessFlags, *fscStatus);

		FSCVirtualFile* dirIterator;

		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR) && redirectTree.getDirectory(path, dirIterator))
			return dirIterator;

		return nullptr;
	}

public:
	static fscDeviceTypeRedirect& instance()
	{
		static fscDeviceTypeRedirect _instance;
		return _instance;
	}
};

bool _redirectMapped = false;

void fscDeviceRedirect_map()
{
	if (_redirectMapped)
		return;
	fsc_mount("/", "/", &fscDeviceTypeRedirect::instance(), nullptr, FSC_PRIORITY_REDIRECT);
	_redirectMapped = true;
}

// Graphic pack replacement files are added when a pack is activated. The Xbox frontend starts titles again in the
// same process, so the redirections of the previous launch must go or a pack turned off in the launcher keeps
// replacing files until Cemu is restarted (#906). Empty directory nodes stay; they only list as empty folders.
void fscDeviceRedirect_reset()
{
	size_t removed = 0;
	for (const auto& path : redirectPaths)
	{
		RedirectEntry* entry;
		if (!redirectTree.getFile(path, entry))
			continue;
		redirectTree.removeFile(path);
		delete entry;
		++removed;
	}
	redirectPaths.clear();
	if (removed)
		cemuLog_log(LogType::Force, "Graphic packs: {} replaced files of the previous launch removed", removed);
}
