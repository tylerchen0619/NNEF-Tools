/*
 * Copyright (c) 2017 The Khronos Group Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <fstream>
#include <cstring>
#include <memory>
#include <regex>
#include <map>
#include "flat/flat_parser.h"
#include "comp/comp_parser.h"
#include "common/binary.h"


struct PrintCallback : public nnef::Parser::Callback
{
    std::map<std::string,bool> atomics;

    PrintCallback( const std::map<std::string,bool>& atomics )
    : atomics(atomics)
    {
    }

    virtual void beginGraph( const nnef::Prototype& proto )
    {
        std::cout << "graph " << proto.name() << "( ";
        
        for ( size_t i = 0; i < proto.paramCount(); ++i )
        {
            auto& param = proto.param(i);
            
            if ( i )
            {
                std::cout << ", ";
            }
            std::cout << param.name();
        }
        
        std::cout << " ) -> ( ";
        
        for ( size_t i = 0; i < proto.resultCount(); ++i )
        {
            auto& result = proto.result(i);
            
            if ( i )
            {
                std::cout << ", ";
            }
            std::cout << result.name();
        }
        
        std::cout << " )" << std::endl << '{' << std::endl;
    }

    virtual void endGraph( const nnef::Prototype& proto, const nnef::Dictionary<nnef::Shape>& shapes )
    {
        std::cout << '}' << std::endl;
    }

    virtual void operation( const nnef::Prototype& proto, const nnef::Dictionary<nnef::Value>& args,
                           const nnef::Dictionary<nnef::Shape>& shapes )
    {
        std::cout << '\t';

        for ( size_t i = 0; i < proto.resultCount(); ++i )
        {
            auto& result = proto.result(i);

            if ( i )
            {
                std::cout << ", ";
            }
            std::cout << args[result.name()];
        }

        std::cout << " = " << proto.name() << "(";

        for ( size_t i = 0; i < proto.paramCount(); ++i )
        {
            auto& param = proto.param(i);

            if ( i )
            {
                std::cout << ", ";
            }
            if ( !param.type()->isTensor() )
            {
                std::cout << param.name() << " = ";
            }
            std::cout << args[param.name()];
        }

        std::cout << ")" << std::endl;
    }
    
    virtual bool isAtomic( const nnef::Prototype& proto, const nnef::Dictionary<nnef::Value>& args )
    {
        auto it = atomics.find(proto.name());
        if ( it != atomics.end() )
        {
            return it->second;
        }
        return nnef::Parser::Callback::isAtomic(proto, args);
    }
};


static std::map<std::string,bool> parseAtomics( const char* str )
{
    std::map<std::string,bool> atomics;

    std::regex reg("\\s+");
    std::cregex_token_iterator it(str, str + strlen(str), reg, -1);
    while ( it != std::cregex_token_iterator() )
    {
        auto token = (*it++).str();

        char ch = token[0];
        if ( ch == '+' || ch == '-' )
        {
            atomics.emplace(token.substr(1), ch == '+');
        }
        else
        {
            std::cout << "atomic op must be marked with '+' or '-' for addition to or removal from the list of standard ops" << std::endl;
        }
    }
    return atomics;
}


int main( int argc, const char * argv[] )
{
    if ( argc < 2 )
    {
        std::cout << "Usage: nnef-validator <network-structure.nnef> [--flat] [--layers] [--binary]" << std::endl;
        std::cout << std::endl;
        std::cout << "Description of options:" << std::endl;
        std::cout << "--flat: use flat parser instead of compositional" << std::endl;
        std::cout << "--layers: enable predefined layer level fragments" << std::endl;
        std::cout << "--binary: check binary data files for variables" << std::endl;
        std::cout << "--atomics: op names to add/remove from atomic ops" << std::endl;
        std::cout << "           default list includes standard ops" << std::endl;
        std::cout << "           e.g. +op1 adds op1, -op2 removes op2" << std::endl;
        return 0;
    }
    
    const std::string filename = argv[1];
    
    std::ifstream is(filename.c_str());
    if ( !is )
    {
        std::cerr << "Could not open file: " << filename << std::endl;
        return -1;
    }

    bool flat = false;
    bool layers = false;
    bool binary = false;
    std::map<std::string,bool> atomics;
    for ( int i = 2; i < argc; ++i )
    {
        if ( std::strcmp(argv[i], "--flat") == 0 )
        {
            flat = true;
        }
        else if ( std::strcmp(argv[i], "--layers") == 0 )
        {
            layers = true;
        }
        else if ( std::strcmp(argv[i], "--binary") == 0 )
        {
            binary = true;
        }
        else if ( std::strcmp(argv[i], "--atomics") == 0 )
        {
            atomics = parseAtomics(argv[++i]);
        }
        else
        {
            std::cerr << "unrecognized option: " << argv[i] << std::endl;
        }
    }

    PrintCallback callback(atomics);
    std::unique_ptr<nnef::Parser> parser(flat ? (nnef::Parser*)new nnef::FlatParser() : (nnef::Parser*)new nnef::CompParser(layers));
    
    try
    {
        parser->parse(is, callback);
        std::cout << "Parse succeeded" << std::endl;
    }
    catch ( nnef::Error e )
    {
        printf("Parse error: [%u:%u] %s\n", e.position().line, e.position().column, e.what());

        auto origin = e.position().origin;
        while ( origin )
        {
            printf("... evaluated from [%u:%u]\n", origin->line, origin->column);
            origin = origin->origin;
        }
    }

    if ( binary )
    {
        for ( auto it : callback.variableShapes() )
        {
            const std::string binaryFilename = filename.substr(0, filename.find_last_of('/') + 1) + it.first + ".dat";
            std::ifstream bin(binaryFilename.c_str(), std::ios::binary);
            if ( !bin )
            {
                std::cerr << "Could not open file: " << binaryFilename << std::endl;
                continue;
            }

            nnef::TensorHeader header;
            if ( !nnef::read_tensor_header(bin, header) )
            {
                std::cerr << "Failed to read binary header from file: " << binaryFilename << std::endl;
                continue;
            }

            if ( header.shape != it.second )
            {
                std::cerr << "Shape " << header.shape << " in tensor file '" << binaryFilename << "' does not match shape "
                << it.second << " defined in network structure" << std::endl;
            }
        }
    }

    return 0;
}
