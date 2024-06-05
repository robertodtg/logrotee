#include <string>
#include <iostream>
#include <inttypes.h>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define BUF_SIZE 4096
#define DEFAULT_CHUNK_SIZE (20*1000*1000)

#pragma ide diagnostic ignored "ClangTidyInspection"

using std::string;

const char *programName = "logrotee";
const char *programVersion = "0.0.1";


struct Arguments {
	string logFilePath;
	string compressCommand;
	string compressSuffix;
	bool nullStdout;
	size_t chunkSize;
	bool dates;
	ssize_t maxFiles;

private:
	bool invalid = false;
	
public:
	Arguments(int argc, char *argv[])
			: chunkSize(DEFAULT_CHUNK_SIZE)
	{
		int getoptResult;
		
		for(;;) {
			int optionIndex = 0;
			static struct option longOptions[] = {
					{"compress", required_argument, nullptr, 'c' },
					{"compress-suffix", required_argument, nullptr, 's' },
					{"null", no_argument, nullptr, 'n' },
					{"dates", no_argument, nullptr, 'd' },
					{"max-files", required_argument, nullptr, 'm'},
					{"chunk", required_argument, nullptr, 'k'},
					{"help", no_argument, nullptr, 'h' },
					{"version", no_argument, nullptr, 'v' },
					{nullptr, 0, nullptr, 0 }
			};
			
			getoptResult = getopt_long(argc, argv, "", longOptions, &optionIndex);
			if (getoptResult == -1)
				break;
			
			switch (getoptResult) {
				case 'c':
					compressCommand = optarg;
					break;
				
				case 's':
					compressSuffix = optarg;
					break;
				
				case 'n':
					nullStdout = true;
					break;
				
				case 'd':
					dates = true;
					break;
				
				case 'm':
					maxFiles = strtoumax(optarg, nullptr, 10);
					if (errno == ERANGE) {
						std::cout << "Cannot parse number: " << optarg << std::endl;
						exit(EXIT_FAILURE);
					}
					break;
				
				case 'k':
					chunkSize = (size_t) strtoumax(optarg, nullptr, 10);
					if (errno != 0) {
						std::cout << "Cannot parse number: " << optarg << std::endl;
						exit(EXIT_FAILURE);
					}
					break;
				
				case 'h':
					usage();
					exit(EXIT_SUCCESS);
				
				case 'v':
					std::cout << programName << " " << programVersion << std::endl;
					exit(EXIT_SUCCESS);
				
				default:
					printf("getopt returned unexpected character 0x%x\n", getoptResult);
			}
		}
		
		if (optind < argc) {
			logFilePath = argv[optind];
			std::cout << logFilePath;
			optind++;
			while (optind < argc) {
				invalid = true;
				std::cerr << "Extra command line argument:" << argv[optind] << std::endl;
				optind++;
			}
		}
	}
	
	bool isInvalid() const {
		return invalid || logFilePath.empty();
	}
	
	void usage() {
		std::cout << "Example usage: verbose_command | logrotee"
				" --compress \"bzip2 {}\" --compress-suffix .bz2"
				" --null --chunk 2M"
				" /var/log/verbose_command.log" << std::endl;
	}
};


class Logrotatee {
	pid_t lastChild;
	FILE *logFile;
	size_t bytesInChunk;
	int nameSuffix;
	const Arguments& commandArgs;
	
public:
	explicit Logrotatee(const Arguments& args)
			: lastChild(0), logFile(nullptr), bytesInChunk(0), nameSuffix(0), commandArgs(args)
	{};
	
	void go();
	
private:
	void rotateLog();
	string getNewChunkName();
};


bool fileExists(const string& name) {
	struct stat sb;
	return stat(name.c_str(), &sb) == 0;
}

string Logrotatee::getNewChunkName() {
	string name;
	char buffer[32];

	snprintf(buffer, 32, ".%d", nameSuffix);
	name = commandArgs.logFilePath + buffer;
	// Hmm, a race condition. Hopefully no one else creates these files, or
	// we'll lose one of them.
	
	if (fileExists(name)) {
		std::remove(name.c_str());
	}

       	if (fileExists(name + commandArgs.compressSuffix)) {
		std::remove((name + commandArgs.compressSuffix).c_str());
	}

	nameSuffix++;

	if (nameSuffix >= commandArgs.maxFiles) {
		nameSuffix = 0;
	}
	
	return name;
}

string replaceSubstring(string str, const string& what, const string& with) {
	size_t startPos = str.find(what);
	if(startPos == string::npos)
		return str;
	str.replace(startPos, what.length(), with);
	return str;
}

int execCompression(const string& command) {
	
	static pid_t lastChild = 0;

//	if (lastChild != 0) {
//		siginfo_t siginfo;
//		siginfo.si_pid = 0;
//		int childStatusChanged = waitid(P_PID, lastChild, &siginfo, WEXITED);
//		if (errno != ECHILD && childStatusChanged != 0) {
//			fprintf(stderr, "Error in waitid(): %d - %s\n", errno, strerror(errno));
//		} else if (siginfo.si_pid != 0) {
//			fprintf(stderr, "Warning: a previous compression process %d==%d has not exited yet.\n", siginfo.si_pid, lastChild);
//		}
//	}
	
	pid_t child;
	for(;;) {
		child = fork();
		if (errno != EAGAIN) {
			break;
		}
		sleep(1);
	}
	
	if (child > 0) {
		lastChild = child;
	} else if (child < 0) {
		fprintf(stderr, "Error %d in fork(%s): %s\n", errno, command.c_str(), strerror(errno));
	} else {
		int status = system(command.c_str());
//		if (status == -1) {
//			fprintf(stderr, "Can't run the command(%s)\n", command.c_str());
//		}
//		else if (WEXITSTATUS(status) != 0) {
//			fprintf(stderr, "Error %d running the command(%s)\n", status, command.c_str());
//		}
		// Child must die without closing file descriptors.
		abort();
	}
	
	return 0;
}

void Logrotatee::rotateLog() {
	const char *logFilePath = commandArgs.logFilePath.c_str();
	
	if (logFile != nullptr) {
		string newName = getNewChunkName();
		fclose(logFile);
		logFile = nullptr;
		rename(logFilePath, newName.c_str());
		
		if (!commandArgs.compressCommand.empty()) {
			string command = replaceSubstring(commandArgs.compressCommand, "{}", newName);
			execCompression(command);
		}
	}
	
	logFile = fopen(logFilePath, "a");
	if (logFile == nullptr) {
		fprintf(stderr, "Error opening %s: %d (%s)\n", logFilePath, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

void Logrotatee::go() {
	char buffer[BUF_SIZE];
	
	rotateLog();
	for(char *s = fgets(buffer, BUF_SIZE, stdin); s != nullptr; s = fgets(buffer, BUF_SIZE, stdin)) {
		fputs(s, logFile);
		if (!commandArgs.nullStdout) {
			fputs(s, stdout);
		}
		
		// As much as we want to avoid extra scanning, we need to know when to rotate logs.
		size_t length = strlen(s);
		bytesInChunk += length;
		if (length > 0 && bytesInChunk >= commandArgs.chunkSize) {
			bool isLineBreak = s[length - 1] == '\n';
			bool breakAnyway = bytesInChunk >= commandArgs.chunkSize * 12 / 10;
			if (isLineBreak || breakAnyway) {
				rotateLog();
				bytesInChunk = 0;
			}
		}
	}
	
	fclose(logFile);
	logFile = nullptr;
}

void ignoreSigchld() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDWAIT;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, nullptr);
}

int main(int argc, char *argv[]) {

	Arguments commandArgs(argc, argv);

	if (commandArgs.isInvalid()) {
		commandArgs.usage();
		exit(EXIT_FAILURE);
	}
	
	ignoreSigchld();
	
	Logrotatee lr(commandArgs);
	lr.go();

	return 0;
}
