#include "compiler_features.h"
#include "cli_args.h"
#include "text_types.h"
#include "repo_cloner.h"
#include "epro_thread.h"
#include "utils.h"

#if EDOPRO_WINDOWS
#define real_main edopro_main
#include "winmain.inl"
#elif (EDOPRO_IOS || EDOPRO_ANDROID)
#define real_main edopro_main
#else
#define real_main main
#endif //EDOPRO_WINDOWS
#if EDOPRO_POSIX
#include <clocale>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif //EDOPRO_POSIX

namespace ygo {

extern epro::thread::id main_thread_id;
epro::thread::id main_thread_id;

}

args_t cli_args;
int edopro_main(const args_t& cli_args);

namespace {
auto GetOption(epro::path_stringview option) {
	if(option.size() == 1) {
		switch(option.front()) {
		case EPRO_TEXT('C'): return LAUNCH_PARAM::WORK_DIR;
		case EPRO_TEXT('m'): return LAUNCH_PARAM::MUTE;
		case EPRO_TEXT('l'): return LAUNCH_PARAM::CHANGELOG;
		case EPRO_TEXT('D'): return LAUNCH_PARAM::DISCORD;
		case EPRO_TEXT('u'): return LAUNCH_PARAM::OVERRIDE_UPDATE_URL;
		case EPRO_TEXT('r'): return LAUNCH_PARAM::REPOS_READ_ONLY;
		case EPRO_TEXT('c'): return LAUNCH_PARAM::ONLY_CLONE_REPOS;
		case EPRO_TEXT('U'): return LAUNCH_PARAM::USER_STORAGE_DIRECTORY;
		case EPRO_TEXT('q'): return LAUNCH_PARAM::EXIT_AFTER;
		case EPRO_TEXT('n'): return LAUNCH_PARAM::SET_NICKNAME;
		case EPRO_TEXT('d'): return LAUNCH_PARAM::SET_DECK;
		default: return LAUNCH_PARAM::COUNT;
		}
	}
	if(option == EPRO_TEXT("i-want-to-be-admin"sv))
		return LAUNCH_PARAM::WANTS_TO_RUN_AS_ADMIN;
	if(option == EPRO_TEXT("replay"sv))
		return LAUNCH_PARAM::REPLAY;
	if(option == EPRO_TEXT("host"sv))
		return LAUNCH_PARAM::HOST;
	if(option == EPRO_TEXT("join"sv))
		return LAUNCH_PARAM::JOIN;
	if(option == EPRO_TEXT("deckbuilder"))
		return LAUNCH_PARAM::DECKBUILDER;
	if(option == EPRO_TEXT("width"))
		return LAUNCH_PARAM::SET_WIDTH;
	if(option == EPRO_TEXT("height"))
		return LAUNCH_PARAM::SET_HEIGHT;
	return LAUNCH_PARAM::COUNT;
}

auto ParseArguments(int argc, epro::path_char* argv[]) {
	args_t res;
	for(int i = 1; i < argc; ++i) {
		epro::path_stringview parameter = argv[i];
		if(parameter.size() < 2)
			break;
		if(parameter[0] == EPRO_TEXT('-')) {
			epro::path_stringview param = parameter.substr(1);
			size_t start = (param.size() > 0 && param[0] == EPRO_TEXT('-')) ? 1 : 0;
			size_t eq_pos = param.find(EPRO_TEXT('='), start);
			epro::path_stringview option = param.substr(start, eq_pos - start);
			auto launch_param = GetOption(option);
			if(launch_param == LAUNCH_PARAM::COUNT)
				continue;
			epro::path_stringview argument;
			if(eq_pos != epro::path_stringview::npos) {
				argument = param.substr(eq_pos + 1);
			} else if(i + 1 < argc) {
				const auto* next = argv[i + 1];
				if(next[0] != EPRO_TEXT('-')) {
					argument = next;
					i++;
				}
			}
			res[launch_param] = {true,  argument};
			continue;
		}
	}
	return res;
}
}

extern "C" int real_main(int argc, epro::path_char** argv) {
	ygo::main_thread_id = ygo::Utils::GetCurrThreadId();
#if EDOPRO_POSIX
	setlocale(LC_CTYPE, "UTF-8");
	struct sigaction sa;
	sa.sa_handler = []([[maybe_unused]] int signum) {
		pid_t pid;
		int status;
		while((pid = waitpid(-1, &status, WNOHANG)) > 0);
	};
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	(void)sigaction(SIGCHLD, &sa, 0);
#endif //EDOPRO_POSIX
	cli_args = ParseArguments(argc, argv);
	if(cli_args[ONLY_CLONE_REPOS].enabled)
		return repo_cloner_main(cli_args);
	return edopro_main(cli_args);
}
