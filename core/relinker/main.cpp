#include <Cli.hpp>
#include <domain/Types.hpp>
#include <io/FileReader.hpp>
#include <io/NativePath.hpp>
#include <elfpatcher/linux/LinuxElfPatcher.hpp>
#include <elfpatcher/linux/LinuxDesktopEntryWriter.hpp>
#include <elfpatcher/general/SegmentFilter.hpp>
#include <elfpatcher/general/EntryStubBuilder.hpp>
#include <elfpatcher/general/ProgramHeaderLayoutBuilder.hpp>
#include <elfpatcher/general/SectionHeaderTableBuilder.hpp>
#include <elfpatcher/windows/WindowsElfPatcher.hpp>
#include <elfpatcher/macos/MacOsMachOPatcher.hpp>
#include <io/ByteWriter.hpp>
#include <io/FileWriter.hpp>
#include <relinker/parsing/ElfReader.hpp>
#include <relinker/analysis/ValidationPolicy.hpp>
#include <relinker/analysis/SyscallScanner.hpp>
#include <relinker/analysis/CallSiteResolver.hpp>
#include <relinker/analysis/UnusedNidFilter.hpp>
#include <relinker/output/SysVDynamicSectionBuilder.hpp>
#include <relinker/output/CallRegistryWriter.hpp>
#include <relinker/pipeline/RelinkerPipeline.hpp>
#include <relinker/guest/GuestImage.hpp>
#include <codegen/IAmd64OnlyConverter.hpp>
#include <map>
#include <codegen/CodegenException.hpp>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif

namespace {

struct ConvertedExecutable {
    std::string InputPath;
    std::string OutputPath;
    std::vector<std::uint8_t> Source;
    std::vector<Codegen::TrampolineSite> Trampolines;
    Relinker::RelinkResult Result;
    std::vector<Relinker::GuestArtifact> GuestArtifacts;
    std::vector<std::uint8_t> Bytes;
};

ConvertedExecutable Convert(const Cli::Args& args, const std::string& inputPath, const std::string& absPath, const bool mainExecutable) {
    Io::FileReader fileReader;
    auto sourceBytes = fileReader.Read(inputPath);

    std::vector<Codegen::TrampolineSite> trampolines;
    if (args.toIntel) {
        const auto segments = Relinker::ElfReader(sourceBytes).ReadProgramHeaders();
        auto converted = Codegen::MakeAmd64OnlyConverter(args.toRosetta ? Codegen::Amd64OnlyTarget::Rosetta : Codegen::Amd64OnlyTarget::Intel)->Convert(std::move(sourceBytes), segments);
        sourceBytes = std::move(converted.Bytes);
        trampolines = std::move(converted.Trampolines);
        std::map<std::string, std::size_t> stubsByName;
        const auto counted = [](const std::string& name) { return name == "VRSQRTPS" || name == "VRCPPS" || name == "256-bit store"; };
        for (const auto& report : converted.Reports) {
            if (report.Lowering == Codegen::Amd64OnlyLowering::Kept)
                std::cout << "Intel substitution: " << report.InstructionName << " at 0x" << std::hex << report.Offset << std::dec << " (" << report.OriginalLength << " bytes) kept: no room for a jump\n";
            else if (counted(report.InstructionName))
                ++stubsByName[report.InstructionName];
            else
                std::cout << "Intel substitution: " << report.InstructionName << " at 0x" << std::hex << report.Offset << std::dec << " (" << report.OriginalLength << " bytes) -> " << (report.Lowering == Codegen::Amd64OnlyLowering::InPlace ? "in place " : "stub ") << report.ReplacementLength << " bytes\n";
        }
        for (const auto& [name, count] : stubsByName)
            std::cout << "Intel substitution: " << name << " -> stub at " << count << " sites\n";
        std::cout << "Intel conversion: " << converted.ReplacedCount << " in place, " << trampolines.size() << " stubs, " << converted.KeptCount << " kept\n";
    }

    auto elfReader = std::make_shared<Relinker::ElfReader>(sourceBytes);
    const std::shared_ptr<Relinker::ISyscallScanner> syscallScanner = args.skipSyscallCheck ? Relinker::MakeNullSyscallScanner() : Relinker::MakeSyscallScanner(args.trapSdkSyscallStubs);

    const auto pipeline = std::make_shared<Relinker::RelinkerPipeline>(
        elfReader,
        syscallScanner,
        Relinker::MakeCallSiteResolver(),
        std::make_shared<Relinker::ValidationPolicy>(),
        std::make_shared<Relinker::SysVDynamicSectionBuilder>(),
        args.unusedFilterLevel == 2 ? Relinker::MakeStrictUnusedNidFilter() : Relinker::MakeUnusedNidFilter(),
        args.unusedFilterLevel
    );

    if (mainExecutable) {
        std::cout << "System: " << (args.toWindows ? "Windows" : args.toMacos ? "macOS" : "Linux") << "; unused-filter=" << args.unusedFilterLevel << "\n";
        std::cout << Relinker::GuestModulePattern << " processing: " << (args.skipSceModule ? "disabled (--skip-sce-module)" : "enabled") << '\n';
        for (const auto& name : args.excludedSceModules) std::cout << "Guest module excluded: " << name << '\n';
    }
    auto result = pipeline->Relink(sourceBytes);
    for (const auto& patch : result.Patches) {
        if (patch.Offset > sourceBytes.size() || patch.Bytes.size() > sourceBytes.size() - patch.Offset)
            throw Domain::RelinkerException("Relinker patch exceeds source image", patch.Offset);
        for (std::size_t index = 0; index < patch.Bytes.size(); ++index) sourceBytes[patch.Offset + index] = patch.Bytes[index];
    }

    std::vector<Relinker::GuestArtifact> guestArtifacts;
    if (!args.skipSceModule) {
        guestArtifacts = Relinker::GuestModuleBuilder().Build(Io::NativePath(inputPath), Io::NativePath(absPath), result.DynamicSection, args.toWindows, args.toMacos, args.toIntel, args.toRosetta, *syscallScanner, args.lazyBinding, args.runPath, args.excludedSceModules, Io::NativePath(args.sceModulePath), args.moduleDirectories);
    }

    return {inputPath, absPath, std::move(sourceBytes), std::move(trampolines), std::move(result), std::move(guestArtifacts), {}};
}

void Patch(const Cli::Args& args, ConvertedExecutable& converted, const bool mainExecutable) {
    auto byteWriter = std::make_shared<Io::ByteWriter>();

    std::shared_ptr<Elfpatcher::IElfPatcher> patcher;
    if (args.toMacos) {
        patcher = std::make_shared<Elfpatcher::MacOs::MacOsMachOPatcher>();
    } else if (args.toWindows) {
        patcher = std::make_shared<Elfpatcher::Windows::WindowsPePatcher>(args.windowsGui, Io::NativePath(args.inputPath).parent_path() / "sce_sys" / "icon0.png");
    } else {
        patcher = std::make_shared<Elfpatcher::Linux::LinuxElfPatcher>(
            std::make_shared<Elfpatcher::EntryStubBuilder>(),
            std::make_shared<Elfpatcher::ProgramHeaderLayoutBuilder>(
                std::make_shared<Elfpatcher::SegmentFilter>(),
                byteWriter
            ),
            std::make_shared<Elfpatcher::SectionHeaderTableBuilder>(byteWriter),
            byteWriter
        );
    }

    try {
        converted.Bytes = patcher->Patch(converted.Source, converted.Result.OriginalHeaders, converted.Result.DynamicSection, converted.Result.OriginalPltGotVaddr, args.runPath, args.lazyBinding, args.windowsDiagnostics && mainExecutable, converted.Trampolines);
    } catch (Domain::RelinkerException& error) {
        error.InputPath = converted.InputPath;
        throw;
    }
}

int Run(const int argc, char* argv[]) {
    Cli::Args args;
    try {
        args = Cli::ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }

    if (args.showHelp) {
        std::cout << Cli::Usage() << '\n';
        return 0;
    }

    try {
        auto extension = Io::Utf8Path(Io::NativePath(args.outputPath).extension());
        for (auto& character : extension) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
        if (!args.toWindows && !args.toMacos && extension == ".exe") std::cerr << "WARNING: Output filename ends with .exe, but --windows was not specified. The output will be a Linux ELF executable.\n";
        Io::FileWriter fileWriter;

        if (std::filesystem::exists(Io::NativePath(args.inputPath)) && std::filesystem::exists(Io::NativePath(args.outputPath)) && std::filesystem::equivalent(Io::NativePath(args.inputPath), Io::NativePath(args.outputPath)))
            throw Domain::RelinkerException("Executable output would overwrite the input executable");
        const std::string absPath = Io::Utf8Path(std::filesystem::absolute(Io::NativePath(args.outputPath)));

        struct ExtraExecutable {
            std::string Argument;
            std::filesystem::path Relative;
            std::filesystem::path Input;
            std::filesystem::path Output;
        };
        std::vector<ExtraExecutable> extraExecutables;
        const auto outputDirectory = Io::NativePath(absPath).parent_path();
        const auto moduleRoot = args.executables.empty() ? std::filesystem::path() : std::filesystem::absolute(Io::NativePath(args.sceModulePath)).lexically_normal();
        std::vector<std::filesystem::path> moduleDirectories;
        for (const auto* name : {"sce_module", "sce_modules", "prx"}) moduleDirectories.push_back(moduleRoot / name);
        for (const auto& name : args.moduleDirectories) moduleDirectories.push_back((moduleRoot / Io::NativePath(name)).lexically_normal());
        for (auto& directory : moduleDirectories) if (!directory.has_filename()) directory = directory.parent_path();
        for (const auto& executable : args.executables) {
            const auto relative = Io::NativePath(executable).lexically_normal();
            if (relative.empty() || relative.has_root_path() || *relative.begin() == ".." || !relative.has_filename())
                throw Domain::RelinkerException("--executable must be a file inside the guest module parent directory: " + executable);
            const auto input = moduleRoot / relative;
            if (!std::filesystem::is_regular_file(input)) throw Domain::RelinkerException("--executable is not a file: " + Io::Utf8Path(input));
            if (std::find(moduleDirectories.begin(), moduleDirectories.end(), input.parent_path()) != moduleDirectories.end())
                throw Domain::RelinkerException("--executable is in a guest module directory: " + executable);
            std::string name;
            for (const auto& part : relative) name += (name.empty() ? "" : ".") + Io::Utf8Path(part);
            const auto output = outputDirectory / Io::NativePath(name + Io::Utf8Path(Io::NativePath(absPath).extension()));
            if (std::filesystem::equivalent(Io::NativePath(args.inputPath), input)) throw Domain::RelinkerException("Executable is converted twice: " + executable);
            if (output.lexically_normal() == Io::NativePath(absPath).lexically_normal()) throw Domain::RelinkerException("Executable outputs collide: " + Io::Utf8Path(output));
            for (const auto& extra : extraExecutables) {
                if (std::filesystem::equivalent(extra.Input, input)) throw Domain::RelinkerException("Executable is converted twice: " + executable);
                if (extra.Output.lexically_normal() == output.lexically_normal()) throw Domain::RelinkerException("Executable outputs collide: " + Io::Utf8Path(output));
            }
            extraExecutables.push_back({executable, relative, input, output});
        }

        std::vector<ConvertedExecutable> executables;
        executables.push_back(Convert(args, args.inputPath, absPath, true));
        for (const auto& [executable, relative, input, output] : extraExecutables) {
            std::cout << "Executable: " << relative.generic_string() << '\n';
            auto converted = Convert(args, Io::Utf8Path(input), Io::Utf8Path(output), false);
            const auto& mainArtifacts = executables.front().GuestArtifacts;
            const bool shared = converted.GuestArtifacts.size() == mainArtifacts.size() && std::equal(mainArtifacts.begin(), mainArtifacts.end(), converted.GuestArtifacts.begin(), [](const auto& left, const auto& right) {
                return left.Path == right.Path && left.Bytes == right.Bytes;
            });
            if (!shared) throw Domain::RelinkerException("Guest modules convert differently for " + executable + ", so it cannot share them with the input executable");
            executables.push_back(std::move(converted));
        }
        const auto& guestArtifacts = executables.front().GuestArtifacts;

        const Elfpatcher::Linux::LinuxDesktopEntryWriter desktopEntryWriter;
        const auto desktopEntry = args.toWindows || args.toMacos ? std::nullopt : desktopEntryWriter.Prepare(Io::NativePath(args.inputPath).parent_path() / "sce_sys", Io::NativePath(absPath));

        const auto checkOutputInputs = [&](const std::filesystem::path& path, const char* kind) {
            if (!std::filesystem::exists(path)) return;
            for (const auto& executable : executables)
                if (std::filesystem::equivalent(Io::NativePath(executable.InputPath), path))
                    throw Domain::RelinkerException(std::string(kind) + " output would overwrite the input executable: " + Io::Utf8Path(path));
            for (const auto& artifact : guestArtifacts)
                if (std::filesystem::equivalent(artifact.SourcePath, path))
                    throw Domain::RelinkerException(std::string(kind) + " output would overwrite an input module: " + Io::Utf8Path(path));
        };
        for (std::size_t index = 1; index < executables.size(); ++index) checkOutputInputs(Io::NativePath(executables[index].OutputPath), "Executable");
        if (desktopEntry) {
            checkOutputInputs(desktopEntry->IconPath, "Icon");
            checkOutputInputs(desktopEntry->EntryPath, "Desktop entry");
        }
        if (args.windowsDiagnostics)
            checkOutputInputs(std::filesystem::absolute("windows-diagnostics-imports.txt"), "Windows import diagnostics");

        std::vector<std::filesystem::path> registryPaths;
        if (args.writeRegistry) {
            for (const auto& executable : executables) {
                const std::filesystem::path outFsPath = Io::NativePath(executable.OutputPath);
                registryPaths.push_back(outFsPath.parent_path() / Io::NativePath(Io::Utf8Path(outFsPath.stem()) + ".registry.json"));
            }
            const std::filesystem::path outFsPath = Io::NativePath(absPath);
            for (const auto& artifact : guestArtifacts)
                registryPaths.push_back(outFsPath.parent_path() / Io::NativePath(Io::Utf8Path(outFsPath.stem()) + "." + Io::Utf8Path(artifact.Path.filename()) + ".registry.json"));
            for (const auto& path : registryPaths) checkOutputInputs(path, "Registry");
        }

        for (std::size_t index = 0; index < executables.size(); ++index) Patch(args, executables[index], index == 0);
        for (const auto& artifact : guestArtifacts) {
            std::filesystem::create_directories(artifact.Path.parent_path());
            fileWriter.Write(Io::Utf8Path(artifact.Path), artifact.Bytes);
            std::cout << "Guest module: " << Io::Utf8Path(artifact.Path) << '\n';
        }
        for (const auto& executable : executables) fileWriter.Write(executable.OutputPath, executable.Bytes);
        if (args.writeRegistry) {
            const auto registryWriter = std::make_shared<Relinker::CallRegistryWriter>();
            for (std::size_t index = 0; index < executables.size(); ++index)
                fileWriter.Write(Io::Utf8Path(registryPaths[index]), registryWriter->WriteCallRegistry(executables[index].Result.RegistryEntries));
            for (std::size_t index = 0; index < guestArtifacts.size(); ++index)
                fileWriter.Write(Io::Utf8Path(registryPaths[executables.size() + index]), registryWriter->WriteModuleImports(guestArtifacts[index].Imports));
        }
        if (desktopEntry) std::cout << "Desktop entry: " << Io::Utf8Path(desktopEntryWriter.Write(*desktopEntry)) << '\n';
        std::cout << "External prx references: " << executables.front().Result.RegistryEntries.size() << "\nOutput file: " << absPath << '\n';
        for (std::size_t index = 1; index < executables.size(); ++index) std::cout << "Executable file: " << executables[index].OutputPath << '\n';
        std::cout << "Expected runtime layout (relative to the output executable):\n";
        for (const auto& executable : executables) std::cout << Io::Utf8Path(Io::NativePath(executable.OutputPath).filename()) << "\n";
        std::cout << "libs/\n    *.prx\napp0/\n    <game resources>\n";
        for (const auto& artifact : guestArtifacts) {
            std::string relative = Io::Utf8Path(artifact.Path.lexically_relative(Io::NativePath(absPath).parent_path() / "app0"));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            std::cout << "    " << relative << '\n';
        }
        std::cout << "Game resources and system libraries must be placed in this layout separately.\n";
        if (args.runPath != "$ORIGIN/libs") std::cout << "Custom library search path (--rpath): " << args.runPath << '\n';

        if (args.autorun) return Cli::Autorun(absPath, args.toWindows);

    } catch (const Domain::RelinkerException& e) {
        std::cerr << "FAIL: " << e.what();
        if (e.FailureOffset != 0) std::cerr << " (offset 0x" << std::hex << e.FailureOffset << ")";
        std::cerr << "\n";
        if (!e.InputPath.empty()) std::cerr << "Input: " << e.InputPath << '\n';
        return 2;
    } catch (const Codegen::CodegenException& e) {
        std::cerr << "FAIL: " << e.what();
        if (e.FailureOffset != 0) std::cerr << " (offset 0x" << std::hex << e.FailureOffset << ")";
        std::cerr << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 2;
    }

    return 0;
}

#ifdef _WIN32

std::string Utf8Argument(const wchar_t* argument) {
    const std::size_t argumentSize = std::wcslen(argument);
    if (argumentSize == 0) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, static_cast<int>(argumentSize), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Cannot encode the command line as UTF-8");
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, static_cast<int>(argumentSize), utf8.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("Cannot encode the command line as UTF-8");
    return utf8;
}

}

int wmain(const int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        arguments.emplace_back();
        for (int index = 1; index < argc; ++index) arguments.push_back(Utf8Argument(argv[index]));
        std::vector<char*> pointers;
        pointers.reserve(arguments.size() + 1);
        for (auto& argument : arguments) pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        return Run(static_cast<int>(arguments.size()), pointers.data());
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

#else

}

int main(const int argc, char* argv[]) {
    return Run(argc, argv);
}

#endif
