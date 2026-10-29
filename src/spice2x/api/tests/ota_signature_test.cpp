#include "util/ota_signature.h"

#include <cassert>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    assert(argc > 1 && (argc - 1) % 2 == 0);
    for (int i = 1; i < argc; i += 2) {
        const auto result = ota::verify_signature(argv[i]);
        std::cout << argv[i] << ": " << result.status;
        if (!result.signer.empty()) {
            std::cout << " (" << result.signer << ")";
        }
        std::cout << " - " << result.detail << '\n';
        assert(result.status == argv[i + 1]);
    }
}
