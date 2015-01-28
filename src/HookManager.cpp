#include "Log.hpp"
#include "HookManager.hpp"

#include <vector>
#include <unordered_map>
#include <boost\algorithm\string.hpp>
#include <windows.h>

namespace ReShade
{
	namespace Hooks
	{
		namespace
		{
			enum class HookType
			{
				Export,
				FunctionHook,
				VTableHook
			};
			struct ModuleExport
			{
				void *Address;
				const char *Name;
				unsigned short Ordinal;
			};
			class CriticalSection
			{
			public:
				struct Lock
				{
					Lock(CriticalSection &cs) : CS(cs)
					{
						EnterCriticalSection(&this->CS.mCS);
					}
					~Lock()
					{
						LeaveCriticalSection(&this->CS.mCS);
					}

					CriticalSection &CS;

				private:
					void operator =(const Lock &);
				};

			public:
				CriticalSection()
				{
					InitializeCriticalSection(&this->mCS);
				}
				~CriticalSection()
				{
					DeleteCriticalSection(&this->mCS);
				}

			private:
				CRITICAL_SECTION mCS;
			} sCS;

			HMODULE sExportHookModule = nullptr;
			boost::filesystem::path sExportHookPath;
			std::vector<std::pair<Hook, HookType>> sHooks;
			std::vector<boost::filesystem::path> sDelayedHookPaths;
			std::unordered_map<Hook::Function, Hook::Function *> sVTableAddresses;

			inline HMODULE GetCurrentModuleHandle()
			{
				HMODULE handle = nullptr;
				GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCTSTR>(&GetCurrentModuleHandle), &handle);

				return handle;
			}
			std::vector<ModuleExport> GetModuleExports(HMODULE handle)
			{
				std::vector<ModuleExport> exports;
				BYTE *imageBase = reinterpret_cast<BYTE *>(handle);
				IMAGE_DOS_HEADER *imageHeader = reinterpret_cast<IMAGE_DOS_HEADER *>(imageBase);
				IMAGE_NT_HEADERS *imageHeaderNT = reinterpret_cast<IMAGE_NT_HEADERS *>(imageBase + imageHeader->e_lfanew);

				if (imageHeader->e_magic != IMAGE_DOS_SIGNATURE || imageHeaderNT->Signature != IMAGE_NT_SIGNATURE || imageHeaderNT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size == 0)
				{
					return exports;
				}

				IMAGE_EXPORT_DIRECTORY *imageExports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(imageBase + imageHeaderNT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

				const std::size_t count = static_cast<std::size_t>(imageExports->NumberOfNames);
				exports.reserve(count);

				for (std::size_t i = 0; i < count; ++i)
				{
					ModuleExport symbol;
					symbol.Ordinal = reinterpret_cast<WORD *>(imageBase + imageExports->AddressOfNameOrdinals)[i] + static_cast<WORD>(imageExports->Base);
					symbol.Name = reinterpret_cast<const char *>(imageBase + (reinterpret_cast<DWORD *>(imageBase + imageExports->AddressOfNames)[i]));

					if (imageExports->NumberOfFunctions > 0)
					{
						symbol.Address = reinterpret_cast<void *>(imageBase + (reinterpret_cast<DWORD *>(imageBase + imageExports->AddressOfFunctions)[symbol.Ordinal - static_cast<WORD>(imageExports->Base)]));
					}
					else
					{
						symbol.Address = nullptr;
					}

					exports.push_back(symbol);
				}

				return exports;
			}
			inline boost::filesystem::path GetModuleFileName(HMODULE handle)
			{
				TCHAR path[MAX_PATH];
				GetModuleFileName(handle, path, MAX_PATH);

				return path;
			}

			bool Install(Hook::Function target, const Hook::Function replacement, HookType method)
			{
				LOG(TRACE) << "Installing hook for '0x" << target << "' with '0x" << replacement << "' using method " << static_cast<int>(method) << " ...";

				Hook hook(target, replacement);
				hook.Trampoline = target;

				Hook::Status status = Hook::Status::Unknown;

				switch (method)
				{
					case HookType::Export:
					{
						status = Hook::Status::Success;
						break;
					}
					case HookType::FunctionHook:
					{
						status = Hook::Install(hook);
						break;
					}
					case HookType::VTableHook:
					{
						DWORD protection = 0;
						Hook::Function *const targetAddress = sVTableAddresses.at(target);
		
						if (VirtualProtect(reinterpret_cast<LPVOID *>(targetAddress), sizeof(Hook::Function), PAGE_READWRITE, &protection) != FALSE)
						{
							*targetAddress = replacement;

							VirtualProtect(reinterpret_cast<LPVOID *>(targetAddress), sizeof(Hook::Function), protection, &protection);

							status = Hook::Status::Success;
						}
						else
						{
							status = Hook::Status::MemoryProtectionFailure;
						}
						break;
					}
				}

				if (status == Hook::Status::Success)
				{
					LOG(TRACE) << "> Succeeded.";

					CriticalSection::Lock lock(sCS);

					sHooks.emplace_back(std::move(hook), method);

					return true;
				}
				else
				{
					LOG(ERROR) << "Failed to install hook for '0x" << target << "' with status code " << static_cast<int>(status) << ".";

					return false;
				}
			}
			bool Install(const HMODULE targetModule, const HMODULE replacementModule, HookType method)
			{
				assert(targetModule != nullptr);
				assert(replacementModule != nullptr);

				// Load export tables
				const std::vector<ModuleExport> targetExports = GetModuleExports(targetModule);
				const std::vector<ModuleExport> replacementExports = GetModuleExports(replacementModule);

				if (targetExports.empty())
				{
					LOG(INFO) << "> Empty export table! Skipped.";

					return false;
				}

				std::size_t counter = 0;
				std::vector<std::pair<Hook::Function, Hook::Function>> matches;
				matches.reserve(replacementExports.size());

				LOG(TRACE) << "> Analyzing export table:";
				LOG(TRACE) << "  +--------------------+---------+----------------------------------------------------+";
				LOG(TRACE) << "  | Address            | Ordinal | Name                                               |";
				LOG(TRACE) << "  +--------------------+---------+----------------------------------------------------+";

				// Analyze export table
				for (const auto &symbol : targetExports)
				{
					if (symbol.Name == nullptr || symbol.Address == nullptr)
					{
						continue;
					}

					// Find appropriate replacement
					const auto begin = replacementExports.cbegin(), end = replacementExports.cend(), it = std::find_if(begin, end, [&symbol](const ModuleExport &it) { return boost::equals(it.Name, symbol.Name); });

					// Filter uninteresting functions
					if (it == end || (boost::equals(symbol.Name, "DXGIReportAdapterConfiguration") || boost::equals(symbol.Name, "DXGIDumpJournal") || boost::starts_with(symbol.Name, "DXGID3D10")))
					{
						LOG(TRACE) << "  | 0x" << std::left << std::setw(16) << symbol.Address << " | " << std::setw(7) << symbol.Ordinal << " | " << std::setw(50) << symbol.Name << std::internal << " |";
					}
					else
					{
						LOG(TRACE) << "  | 0x" << std::left << std::setw(16) << symbol.Address << " | " << std::setw(7) << symbol.Ordinal << " | " << std::setw(50) << symbol.Name << std::internal << " | <";

						matches.push_back(std::make_pair(reinterpret_cast<Hook::Function>(symbol.Address), reinterpret_cast<Hook::Function>(it->Address)));
					}
				}

				LOG(TRACE) << "  +--------------------+---------+----------------------------------------------------+";
				LOG(INFO) << "> Found " << matches.size() << " match(es). Installing ...";

				// Hook matching exports
				for (const auto &match : matches)
				{
					if (Install(match.first, match.second, method))
					{
						counter++;
					}
				}

				if (counter != 0)
				{
					LOG(INFO) << "> Installed " << counter << " hook(s).";

					return true;
				}
				else
				{
					LOG(WARNING) << "> Installed 0 hook(s).";

					return false;
				}
			}
			bool Uninstall(Hook &hook, HookType method)
			{
				LOG(TRACE) << "Uninstalling hook for '0x" << hook.Target << "' ...";

				if (!hook.IsInstalled())
				{
					LOG(TRACE) << "> Already uninstalled.";

					return true;
				}
				else if (method == HookType::Export)
				{
					LOG(TRACE) << "> Skipped.";

					return true;
				}

				Hook::Status status = Hook::Status::Unknown;

				switch (method)
				{
					case HookType::FunctionHook:
					{
						status = hook.Uninstall();
						break;
					}
					case HookType::VTableHook:
					{
						DWORD protection = 0;
						Hook::Function *const targetAddress = sVTableAddresses.at(hook.Target);
		
						if (VirtualProtect(reinterpret_cast<LPVOID *>(targetAddress), sizeof(Hook::Function), PAGE_READWRITE, &protection) != FALSE)
						{
							*targetAddress = hook.Target;
							sVTableAddresses.erase(hook.Target);

							VirtualProtect(reinterpret_cast<LPVOID *>(targetAddress), sizeof(Hook::Function), protection, &protection);

							status = Hook::Status::Success;
						}
						else
						{
							status = Hook::Status::MemoryProtectionFailure;
						}
						break;
					}
				}

				if (status == Hook::Status::Success)
				{
					LOG(TRACE) << "> Succeeded.";

					hook.Trampoline = nullptr;

					return true;
				}
				else
				{
					LOG(WARNING) << "Failed to uninstall hook for '0x" << hook.Target << "' with status code " << static_cast<int>(status) << ".";

					return false;
				}
			}

			HMODULE WINAPI HookLoadLibraryA(LPCSTR lpFileName)
			{
				const HMODULE handle = Call(&HookLoadLibraryA)(lpFileName);

				if (handle == nullptr)
				{
					return nullptr;
				}

				CriticalSection::Lock lock(sCS);
				const boost::filesystem::path path = lpFileName;
				const auto begin = sDelayedHookPaths.begin(), end = sDelayedHookPaths.end(), it = std::find_if(begin, end, [&path](const boost::filesystem::path &it) { return boost::iequals(it.stem().native(), path.stem().native()); });

				if (it != end)
				{
					LOG(INFO) << "Installing delayed hooks for " << *it << " (Just loaded via 'LoadLibraryA(" << path << ")') ...";

					if (Install(handle, GetCurrentModuleHandle(), HookType::FunctionHook))
					{
						sDelayedHookPaths.erase(it);
					}
				}

				return handle;
			}
			HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpFileName)
			{
				const HMODULE handle = Call(&HookLoadLibraryW)(lpFileName);

				if (handle == nullptr)
				{
					return nullptr;
				}

				CriticalSection::Lock lock(sCS);
				const boost::filesystem::path path = lpFileName;
				const auto begin = sDelayedHookPaths.begin(), end = sDelayedHookPaths.end(), it = std::find_if(begin, end, [&path](const boost::filesystem::path &it) { return boost::iequals(it.stem().native(), path.stem().native()); });

				if (it != end)
				{
					LOG(INFO) << "Installing delayed hooks for " << *it << " (Just loaded via 'LoadLibraryW(" << path << ")') ...";

					if (Install(handle, GetCurrentModuleHandle(), HookType::FunctionHook))
					{
						sDelayedHookPaths.erase(it);
					}
				}

				return handle;
			}
		}

		// -----------------------------------------------------------------------------------------------------

		void Register(const boost::filesystem::path &targetPath) // Unsafe
		{
			Install(reinterpret_cast<Hook::Function>(&LoadLibraryA), reinterpret_cast<Hook::Function>(&HookLoadLibraryA));
			Install(reinterpret_cast<Hook::Function>(&LoadLibraryW), reinterpret_cast<Hook::Function>(&HookLoadLibraryW));

			LOG(INFO) << "Registering hooks for " << targetPath << " ...";

			const HMODULE replacementModule = GetCurrentModuleHandle();
			const boost::filesystem::path replacementPath = GetModuleFileName(replacementModule);

			if (boost::iequals(targetPath.stem().native(), replacementPath.stem().native()))
			{
				LOG(INFO) << "> Delayed.";

				sExportHookPath = targetPath;
			}
			else
			{
				const HMODULE targetModule = GetModuleHandle(targetPath.c_str());

				if (targetModule != nullptr)
				{
					LOG(INFO) << "> Libraries loaded.";

					Install(targetModule, replacementModule, HookType::FunctionHook);
				}
				else
				{
					LOG(INFO) << "> Delayed.";

					sDelayedHookPaths.push_back(targetPath);
				}
			}
		}

		bool Install(Hook::Function target, const Hook::Function replacement)
		{
			assert(target != nullptr);
			assert(replacement != nullptr);

			if (target == replacement)
			{
				return false;
			}

			const Hook hook = Find(replacement);

			if (hook.IsInstalled())
			{
				return target == hook.Target;
			}

			return Install(target, replacement, HookType::FunctionHook);
		}
		bool Install(Hook::Function vtable[], unsigned int offset, const Hook::Function replacement)
		{
			assert(vtable != nullptr);
			assert(replacement != nullptr);

			DWORD protection = 0;
		
			if (VirtualProtect(reinterpret_cast<LPVOID>(vtable + offset), sizeof(Hook::Function), PAGE_READONLY, &protection) != FALSE)
			{
				const Hook::Function target = vtable[offset];
				const auto insert = sVTableAddresses.emplace(target, vtable + offset);

				VirtualProtect(reinterpret_cast<LPVOID>(vtable + offset), sizeof(Hook::Function), protection, &protection);

				if (insert.second)
				{
					if (target != replacement && Install(target, replacement, HookType::VTableHook))
					{
						return true;
					}
					else
					{
						sVTableAddresses.erase(insert.first);
					}
				}
				else
				{
					return insert.first->first == target;
				}
			}

			return false;
		}
		void Uninstall()
		{
			CriticalSection::Lock lock(sCS);

			LOG(INFO) << "Uninstalling " << sHooks.size() << " hook(s) ...";

			// Uninstall hooks
			for (auto &it : sHooks)
			{
				Uninstall(it.first, it.second);
			}

			sHooks.clear();

			// Free loaded module
			if (sExportHookModule != nullptr)
			{
				FreeLibrary(sExportHookModule);
			}
		}

		Hook Find(const Hook::Function replacement)
		{
			CriticalSection::Lock lock(sCS);

			// Lookup hook
			const auto begin = sHooks.begin(), end = sHooks.end(), it = std::find_if(begin, end, [&replacement](const std::pair<Hook, HookType> &it) { return it.first.Replacement == replacement; });

			if (it != end)
			{
				return it->first;
			}

			return Hook();
		}
		Hook::Function Call(const Hook::Function replacement)
		{
			CriticalSection::Lock lock(sCS);

			if (!sExportHookPath.empty())
			{
				LOG(INFO) << "Installing delayed hooks for " << sExportHookPath << " ...";

				assert(sExportHookModule == nullptr);

				Find(&HookLoadLibraryW).Enable(false);
				sExportHookModule = LoadLibraryW(sExportHookPath.c_str());
				Find(&HookLoadLibraryW).Enable(true);

				if (sExportHookModule != nullptr)
				{
					sExportHookPath.clear();

					Install(sExportHookModule, GetCurrentModuleHandle(), HookType::Export);
				}
				else
				{
					LOG(ERROR) << "Failed to load " << sExportHookPath << "!";
				}
			}

			const Hook hook = Find(replacement);

			if (!hook.IsValid())
			{
				LOG(ERROR) << "Unable to resolve hook for '0x" << replacement << "'!";

				return nullptr;
			}

			return hook.Call();
		}
	}
}