#include <vector>
#include <string>
#include <cstdio>
#include <unistd.h>
#include <gtest/gtest.h>

#include "cli.h"

// Builds an argv[] pointing at the given strings (argv[0] is a stand-in
// program name), suitable for feeding into cli.parse().
class Argv {
public:
	explicit Argv(std::vector<std::string> args) : storage(std::move(args)) {
		storage.insert(storage.begin(), "IN10dfsd");
		for (auto& s : storage)
			ptrs.push_back(s.data());
		argc = (int)ptrs.size();
	}
	int argc;
	char** argv() { return ptrs.data(); }
	// cli.parse() shuffles the char* entries in place (as it must, to
	// behave the same way it would on a real argv passed to main());
	// it does not touch the strings ptrs[i] point into. So "what's left"
	// has to be read back through ptrs, not the original insertion order
	// still sitting in storage.
	std::vector<std::string> remaining() const {
		std::vector<std::string> out;
		for (int i = 0; i < argc; i++)
			out.push_back(ptrs[i]);
		return out;
	}
private:
	std::vector<std::string> storage;
	std::vector<char*> ptrs;
};

class CliTest : public ::testing::Test {
protected:
	void SetUp() override {
		// these are process-wide globals; start every case from the
		// documented defaults so tests don't depend on run order
		cli.gpio_pin = GPIO_LINE_DEFAULT;
		cli.soft_start = false;
		cli.data_path.clear();
		cli.help_requested = false;
	}
};

// --- --gpio-pin ---------------------------------------------------------

TEST_F(CliTest, DefaultsAreUnchangedWhenNoFlagsGiven)
{
	Argv a({"-f", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, GPIO_LINE_DEFAULT);
	EXPECT_FALSE(cli.soft_start);
	EXPECT_TRUE(cli.data_path.empty());
	EXPECT_FALSE(cli.help_requested);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-f", "/mnt/1wire"}));
}

TEST_F(CliTest, GpioPinEqualsFormSetsPin)
{
	Argv a({"--gpio-pin=17", "-f", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, 17);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-f", "/mnt/1wire"}));
}

TEST_F(CliTest, GpioPinTwoTokenFormSetsPin)
{
	Argv a({"--gpio-pin", "12", "-f", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, 12);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-f", "/mnt/1wire"}));
}

TEST_F(CliTest, GpioPinZeroDeactivatesGpio)
{
	Argv a({"--gpio-pin=0"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, 0);
}

TEST_F(CliTest, GpioPinNonNumericValueIsRejectedButStillConsumed)
{
	Argv a({"--gpio-pin=abc", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, GPIO_LINE_DEFAULT);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "/mnt/1wire"}));
}

TEST_F(CliTest, GpioPinNegativeValueIsRejected)
{
	Argv a({"--gpio-pin=-1"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, GPIO_LINE_DEFAULT);
}

TEST_F(CliTest, GpioPinMissingValueAtEndOfArgvIsRejected)
{
	Argv a({"-f", "--gpio-pin"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, GPIO_LINE_DEFAULT);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-f"}));
}

TEST_F(CliTest, SimilarlyPrefixedLongFlagIsLeftAlone)
{
	// --gpio-pin-foo is a different option and must not be mistaken for
	// --gpio-pin, nor have any part of it stripped
	Argv a({"--gpio-pin-foo", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, GPIO_LINE_DEFAULT);
	EXPECT_EQ(a.remaining(),
		std::vector<std::string>({"IN10dfsd", "--gpio-pin-foo", "/mnt/1wire"}));
}

// --- -s / --soft ---------------------------------------------------------

TEST_F(CliTest, ShortSoftFlagIsRecognized)
{
	Argv a({"-s", "-f", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_TRUE(cli.soft_start);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-f", "/mnt/1wire"}));
}

TEST_F(CliTest, LongSoftFlagIsRecognized)
{
	Argv a({"--soft", "-f", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_TRUE(cli.soft_start);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-f", "/mnt/1wire"}));
}

TEST_F(CliTest, SoftFlagTakesNoValueEvenIfFollowedByOne)
{
	// -s is a bare flag: the next token is a separate argument (e.g. the
	// mount point), not a value that gets swallowed
	Argv a({"-s", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_TRUE(cli.soft_start);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "/mnt/1wire"}));
}

TEST_F(CliTest, SimilarlyPrefixedFlagDoesNotSetSoft)
{
	// -soft (not --soft, not -s) must not be mistaken for either form
	Argv a({"-soft", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_FALSE(cli.soft_start);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-soft", "/mnt/1wire"}));
}

// --- -d / --data ---------------------------------------------------------

TEST_F(CliTest, ShortDataEqualsFormSetsPath)
{
	Argv a({"-d=/etc/in10d/data.json", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.data_path, "/etc/in10d/data.json");
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "/mnt/1wire"}));
}

TEST_F(CliTest, ShortDataTwoTokenFormSetsPath)
{
	Argv a({"-d", "/etc/in10d/data.json", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.data_path, "/etc/in10d/data.json");
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "/mnt/1wire"}));
}

TEST_F(CliTest, LongDataEqualsFormSetsPath)
{
	Argv a({"--data=/etc/in10d/data.json", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.data_path, "/etc/in10d/data.json");
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "/mnt/1wire"}));
}

TEST_F(CliTest, LongDataTwoTokenFormSetsPath)
{
	Argv a({"--data", "/etc/in10d/data.json", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.data_path, "/etc/in10d/data.json");
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "/mnt/1wire"}));
}

TEST_F(CliTest, DataMissingValueAtEndOfArgvIsRejected)
{
	Argv a({"-f", "--data"});
	cli.parse(a.argc, a.argv());

	EXPECT_TRUE(cli.data_path.empty());
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-f"}));
}

// --- -h / --help -----------------------------------------------------

TEST_F(CliTest, ShortHelpFlagIsRecognizedAndLeftInArgv)
{
	// -h/--help is deliberately NOT stripped: fuse_main() needs to see
	// it too, so it prints its own help for FUSE's own options
	Argv a({"-h", "-f", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_TRUE(cli.help_requested);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-h", "-f", "/mnt/1wire"}));
}

TEST_F(CliTest, LongHelpFlagIsRecognizedAndLeftInArgv)
{
	Argv a({"--help"});
	cli.parse(a.argc, a.argv());

	EXPECT_TRUE(cli.help_requested);
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "--help"}));
}

TEST_F(CliTest, HelpDoesNotPreventOtherOptionsFromStillBeingParsed)
{
	Argv a({"--gpio-pin=9", "-h", "-s"});
	cli.parse(a.argc, a.argv());

	EXPECT_TRUE(cli.help_requested);
	EXPECT_EQ(cli.gpio_pin, 9);
	EXPECT_TRUE(cli.soft_start);
	// --gpio-pin and -s are still consumed as usual; only -h stays
	EXPECT_EQ(a.remaining(), std::vector<std::string>({"IN10dfsd", "-h"}));
}

TEST_F(CliTest, PrintHelpMentionsEveryOption)
{
	// print_help() has no branches to speak of, so this is mostly about
	// coverage and a sanity check that nothing was renamed without
	// updating the text - not a layout/formatting assertion. Redirects
	// through a real temp file rather than fmemopen(), since fmemopen
	// streams aren't reliably fd-backed for a dup2() trick.
	FILE* tmp = tmpfile();
	ASSERT_NE(tmp, nullptr);

	int saved_stdout = dup(fileno(stdout));
	ASSERT_GE(saved_stdout, 0);
	fflush(stdout);
	dup2(fileno(tmp), fileno(stdout));

	cli.print_help("IN10dfsd");

	fflush(stdout);
	dup2(saved_stdout, fileno(stdout));
	close(saved_stdout);

	rewind(tmp);
	char buf[4096] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, tmp);
	buf[n] = '\0';
	fclose(tmp);

	std::string text(buf);
	EXPECT_NE(text.find("IN10dfsd"), std::string::npos);
	EXPECT_NE(text.find("--gpio-pin"), std::string::npos);
	EXPECT_NE(text.find("--soft"), std::string::npos);
	EXPECT_NE(text.find("--data"), std::string::npos);
	EXPECT_NE(text.find("--help"), std::string::npos);
}

// --- combined --------------------------------------------------------

TEST_F(CliTest, AllThreeOptionsTogetherInAnyOrder)
{
	Argv a({"-f", "--data=/data/cfg.json", "--gpio-pin=22", "-o", "allow_other",
		"-s", "/mnt/1wire"});
	cli.parse(a.argc, a.argv());

	EXPECT_EQ(cli.gpio_pin, 22);
	EXPECT_TRUE(cli.soft_start);
	EXPECT_EQ(cli.data_path, "/data/cfg.json");
	EXPECT_EQ(a.remaining(),
		std::vector<std::string>({"IN10dfsd", "-f", "-o", "allow_other", "/mnt/1wire"}));
}
