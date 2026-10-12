// pearch — instant file search for your terminal.

#include "app/cli.h"
#include "app/commands.h"
#include "platform/shell.h"

int main(int argc, char** argv) {
    shell::init();
    return app::run(app::parse(shell::utf8_args(argc, argv)));
}
