//
// Created by AI Assistant on 2024/12/25.
// Copyright (c) 2024 Alibaba Group Holding Limited All rights reserved.
//

#ifndef USER_INTERFACE_HPP
#define USER_INTERFACE_HPP

#include <string>
#include <iostream>
#include <iomanip>

namespace mnncli {

// User interface utilities
class UserInterface {
public:
    static void ShowWelcome() {
        std::cout << "🚀 MNN CLI - MNN Command Line Interface\n";
        std::cout << "Type 'mnncli --help' for available commands\n\n";
    }
    
    static void ShowProgress(const std::string& message, float progress) {
        int bar_width = 50;
        int pos = static_cast<int>(bar_width * progress);
        
        // Clear the line first to prevent leftover characters
        std::cout << "\r\033[K" << message << " [";
        for (int i = 0; i < bar_width; ++i) {
            if (i < pos) {
                std::cout << "█";  // Filled block
            } else if (i == pos) {
                std::cout << "▶";  // Arrow
            } else {
                std::cout << "░";  // Light block
            }
        }
        // Add padding to prevent leftover characters
        std::cout << "] " << std::fixed << std::setprecision(1) << (progress * 100.0) << "%" 
                  << std::string(3, ' ') 
                  << std::flush;
        
        if (progress >= 1.0) std::cout << std::endl;
    }
    
    static void ShowError(const std::string& error, const std::string& suggestion = "") {
        std::cerr << "❌ Error: " << error << std::endl;
        if (!suggestion.empty()) {
            std::cerr << "💡 Suggestion: " << suggestion << std::endl;
        }
    }
    
    static void ShowSuccess(const std::string& message) {
        std::cout << "✅ " << message << std::endl;
    }
    
    static void ShowInfo(const std::string& message) {
        std::cout << "ℹ️  " << message << std::endl;
    }
};

} // namespace mnncli

#endif // USER_INTERFACE_HPP
