#include "parser.h"

#define P_DEBUG false

// To assist in error printing 
const char* TokenTypeStrings[] = 
{
".", ";", "(", ")", ",", "[", "]", ":", "&", "|", "+", "-", "<", ">", "<=", ">=", ":=", "==", "!=", "*", "/", "FILE_END", "STRING", "CHAR", "INTEGER", "FLOAT", "BOOL", "IDENTIFIER", "UNKNOWN",
"RS_IN", "RS_OUT", "RS_INOUT", "RS_PROGRAM", "RS_IS", "RS_BEGIN", "RS_END", "RS_GLOBAL", "RS_PROCEDURE", "RS_STRING", "RS_CHAR", "RS_INTEGER", "RS_FLOAT", "RS_BOOL", "RS_IF", "RS_THEN", "RS_ELSE", "RS_FOR", "RS_RETURN", "RS_TRUE", "RS_FALSE", "RS_NOT"
};

// Initialize llvm stuff
using namespace llvm;
using namespace llvm::sys;

static llvm::LLVMContext TheContext;
static llvm::IRBuilder<> Builder(TheContext);
static std::unique_ptr<llvm::Module> TheModule;

Parser::Parser(ErrHandler* handler, SymbolTableManager* manager, Scanner* scan, std::string filename)
    : err_handler(handler), symtable_manager(manager), scanner(scan)
{ 
    // Initialize curr_token so old values aren't used 
    curr_token.type = UNKNOWN;
    curr_token.val.sym_type = S_UNDEFINED;

    TheModule = make_unique<Module>("my IR", TheContext);
}

Parser::~Parser() { }

TokenType Parser::token()
{
    if (curr_token.type == TokenType::FILE_END)
    {
        // Possibly desynchronized - this would go on infinitely
    }
    if (!curr_token_valid)
    {
        curr_token_valid = true;
        curr_token = scanner->getToken();
        if (P_DEBUG) std::cout << "\tGot: " << TokenTypeStrings[curr_token.type] << '\n';
        return curr_token.type;
    }
    else return curr_token.type;
}

Token Parser::advance()
{
    // Mark as consumed
    curr_token_valid = false;
    // Return the current token to be used before getting next token 
    return curr_token;
}

// Require that the current token is of type t
Token Parser::require(TokenType expected_type, bool error)
{
    TokenType type = token();
    advance();
    if (expected_type != type) 
    {
        // Report err
        std::ostringstream stream;
        stream << "Bad Token: " << TokenTypeStrings[type] 
            << "\tExpected: " << TokenTypeStrings[expected_type];
        if (error) err_handler->reportError(stream.str(), curr_token.line);
        else err_handler->reportWarning(stream.str(), curr_token.line);
    }
    return curr_token;
}

void Parser::decl_single_builtin(std::string name, Type* paramtype)
{
    std::vector<Type*> Params(1, paramtype);
    FunctionType *FT =
        FunctionType::get(Type::getVoidTy(TheContext), Params, false);
    Function *F =
        Function::Create(FT, Function::ExternalLinkage, name, TheModule.get());

    // Associate the LLVM function we created with its symboltable entry
    SymTableEntry* entry = symtable_manager->resolve_symbol(name, true);
    entry->function = F;
}

void Parser::decl_builtins()
{
    decl_single_builtin("PUTINTEGER", Type::getInt32PtrTy(TheContext));
    decl_single_builtin("PUTFLOAT", Type::getFloatPtrTy(TheContext));
    decl_single_builtin("PUTCHAR", Type::getInt8PtrTy(TheContext));
    decl_single_builtin("PUTSTRING", Type::getInt8PtrTy(TheContext));
    decl_single_builtin("PUTBOOL", Type::getInt1PtrTy(TheContext));

    decl_single_builtin("GETINTEGER", Type::getInt32PtrTy(TheContext));
    decl_single_builtin("GETFLOAT", Type::getFloatPtrTy(TheContext));
    decl_single_builtin("GETCHAR", Type::getInt8PtrTy(TheContext));
    decl_single_builtin("GETSTRING", Type::getInt8PtrTy(TheContext)->getPointerTo());
    decl_single_builtin("GETBOOL", Type::getInt1PtrTy(TheContext));
}

Value* Parser::convert_type(Value* val, Type* required_type)
{
    Value* retval;

    if (required_type == val->getType() || required_type == nullptr) return nullptr;

    if (required_type == Type::getInt32Ty(TheContext)
             && val->getType() == Type::getFloatTy(TheContext))
    {
        retval = Builder.CreateFPToSI(val, required_type);
    }
    else if (required_type == Type::getFloatTy(TheContext)
            && val->getType() == Type::getInt32Ty(TheContext))
    {
        retval = Builder.CreateSIToFP(val, required_type);
    }
    else if (required_type == Type::getInt1Ty(TheContext) 
            && val->getType() == Type::getInt32Ty(TheContext))
    {
        // Compare to 0 to convert to bool
        retval = Builder.CreateICmpNE(val, 
                ConstantInt::get(TheContext, APInt(1, 0)));
    }
    else if (required_type == Type::getInt32Ty(TheContext) 
            && val->getType() == Type::getInt1Ty(TheContext))
    {
        retval = Builder.CreateZExt(val, required_type);
    }
    else if (required_type == Type::getInt8PtrTy(TheContext)
            && val->getType()->getContainedType(0)->getArrayElementType() 
                == Type::getInt8Ty(TheContext))
    {
        // String (required: i8*, actual: [? x i8]*)
        // Deref to get [? x i8] which is equivalent to i8*
        const std::vector<Value*> GEPIdxs 
            {ConstantInt::get(TheContext, APInt(64, 0)), 
                ConstantInt::get(TheContext, APInt(64, 0))};
        retval = Builder.CreateGEP(val, ArrayRef<Value*>(GEPIdxs));
    }
    else 
    {
        TheModule->print(llvm::errs(), nullptr);

        std::string str;
        raw_string_ostream rso(str);

        rso << "Conflicting types in conversion: req'd: ";
        required_type->print(rso, false);
        rso << " got: ";
        val->getType()->print(rso, false);

        rso.flush();

        err_handler->reportError(str, curr_token.line);
        return nullptr;
    }

    return retval;
}

std::unique_ptr<llvm::Module> Parser::parse() 
{
    program();
    return std::move(TheModule);
}

void Parser::program()
{
    if (P_DEBUG) std::cout << "program" << '\n';

    // Declare builtin functions in llvm file
    decl_builtins();

    // Use main for outer program.
    // Build a simple IR
    // Set up function main (returns i32, no params)
    std::vector<Type *> Parameters;
    FunctionType *FT =
        FunctionType::get(Type::getInt32Ty(TheContext), Parameters, false);
    Function* main =
        Function::Create(FT, Function::ExternalLinkage, "main", TheModule.get());

    symtable_manager->set_curr_proc_function(main);

    // Create basic block of main
    BasicBlock *bb = BasicBlock::Create(TheContext, "entry", main);
    Builder.SetInsertPoint(bb);

    program_header(); 
    program_body(); 
    require(TokenType::PERIOD, false);

    // Return 0 from the main function always
    Value *val = ConstantInt::get(TheContext, APInt(32, 0));
    Builder.CreateRet(val);
}

void Parser::program_header()
{
    if (P_DEBUG) std::cout << "program header" << '\n';

    require(TokenType::RS_PROGRAM);

    require(TokenType::IDENTIFIER);
    std::string program_name = curr_token.val.string_value;

    require(TokenType::RS_IS);
}

void Parser::program_body()
{
    if (P_DEBUG) std::cout << "program body" << '\n';

    bool declarations = true;
    while (true)
    {
        if (token() == TokenType::RS_BEGIN)
        {
            advance();
            declarations = false;
            continue;
        }
        else if (token() == TokenType::RS_END)
        {
            advance();
            require(TokenType::RS_PROGRAM);
            return;
        }
        
        if (declarations) declaration();
        else statement();

        require(TokenType::SEMICOLON);
    }
}

void Parser::declaration()
{
    if (P_DEBUG) std::cout << "declaration" << '\n';

    bool is_global = false;
    if (token() == TokenType::RS_GLOBAL)
    {
        is_global = true;
        advance();
    }

    if (token() == TokenType::RS_PROCEDURE)
    {
        proc_declaration(is_global);
    }
    else var_declaration(is_global); // typemark - continue into var decl
}

void Parser::proc_declaration(bool is_global)
{
    if (P_DEBUG) std::cout << "proc decl" << '\n';

    proc_header();
    proc_body();

    Builder.CreateRetVoid();

    // Reset to scope above this proc decl
    symtable_manager->reset_scope();

    // Get previous IP from proc manager
    //  and restore it so the builder appends to it again
    Builder.restoreIP(symtable_manager->get_insert_point());
}

void Parser::proc_header()
{
    if (P_DEBUG) std::cout << "proc header" << '\n';
    require(TokenType::RS_PROCEDURE);

    // Setup symbol table so the procedure's sym table is now being used
    std::string proc_id = require(TokenType::IDENTIFIER).val.string_value;

    IRBuilderBase::InsertPoint ip = Builder.saveIP();
    symtable_manager->save_insert_point(ip);

    // Sets the current scope to this procedure's scope
    symtable_manager->set_proc_scope(proc_id);

    // Parse params
    require(TokenType::L_PAREN);
    if (token() != TokenType::R_PAREN)
        parameter_list(); 
    require(TokenType::R_PAREN);

    std::vector<SymTableEntry*> params_vec 
        = symtable_manager->get_current_proc_params();

    std::vector<Type*> param_type_vec;

    for (auto param : params_vec)
    {
        Type* param_type;
        switch (param->sym_type)
        {
        case S_INTEGER:
            // If param is type IN, pass by val
            if (param->param_type == RS_IN)
                param_type = Type::getInt32Ty(TheContext);
            else
                param_type = Type::getInt32PtrTy(TheContext);
            break;
        case S_FLOAT:
            if (param->param_type == RS_IN)
                param_type = Type::getFloatTy(TheContext);
            else
                param_type = Type::getFloatPtrTy(TheContext);
            break;
        case S_CHAR:
            if (param->param_type == RS_IN)
                param_type = Type::getInt8Ty(TheContext);
            else
                param_type = Type::getInt8PtrTy(TheContext);
            break;
        case S_BOOL:
            if (param->param_type == RS_IN)
                param_type = Type::getInt1Ty(TheContext);
            else
                param_type = Type::getInt1PtrTy(TheContext);
            break;
        case S_STRING:
            // Strings must be i8* (char*), but if they are 
            //  OUT or INOUT, then they are i8** because the
            //  string that is pointed to by the variable can
            //  be modified, not just the string itself
            param_type = Type::getInt8PtrTy(TheContext);
            if (param->param_type != RS_IN)
                param_type = param_type->getPointerTo();
            break;
        default:
            err_handler->reportError("Invalid symbol type", curr_token.line);
            break;
        }
        if (param->is_arr) 
        {
            if (PointerType* pointer_ty = dyn_cast<PointerType>(param_type))
            {
                param_type = pointer_ty->getElementType();
                param_type = ArrayType::get(param_type, param->arr_size);
                param_type = param_type->getPointerTo();
            }
            else param_type = ArrayType::get(param_type, param->arr_size);
        }
        param_type_vec.push_back(param_type);
    }

    FunctionType *FT =
        FunctionType::get(Type::getVoidTy(TheContext), param_type_vec, false);

    Function* F = Function::Create(FT, 
        Function::ExternalLinkage, 
        proc_id, 
        TheModule.get());

    // Ensure function is valid in IR 
    verifyFunction(*F);

    symtable_manager->set_curr_proc_function(F);

    // Set IP to this function's basic block
    BasicBlock *bb = BasicBlock::Create(TheContext, "entry", F);
    Builder.SetInsertPoint(bb);

    // Set arg names to their real ids
    // Also, allocate pointers for pass-by-val params
    int k = 0;
    for (auto &arg : F->args())
    {
        if (isa<PointerType>(arg.getType()) || isa<ArrayType>(arg.getType()))
        {
            // arg is already a pointer type
            params_vec[k]->value = &arg;
        }
        else
        {
            // arg is a non-pointer type. 
            // Allocate to pointer to use as a var
            AllocaInst* ptr_arg = Builder.CreateAlloca(arg.getType());
            // Store the argument value into the pointer
            Builder.CreateStore(&arg, ptr_arg);
            params_vec[k]->value = ptr_arg;
        }
        arg.setName(params_vec[k++]->id);
    }
}

void Parser::proc_body()
{
    if (P_DEBUG) std::cout << "proc body" << '\n';
    bool declarations = true;
    while (true)
    {
        if (token() == TokenType::RS_BEGIN)
        {
            advance();
            declarations = false;
            continue;
        }
        else if (token() == TokenType::RS_END)
        {
            advance();
            require(TokenType::RS_PROCEDURE);
            return;
        }
        
        if (declarations) declaration();
        else statement();

        require(TokenType::SEMICOLON);
    }
}

void Parser::parameter_list()
{
    if (P_DEBUG) std::cout << "param list" << '\n';
    while (true)
    {
        parameter(); 
        if (token() == TokenType::COMMA)
        {
            advance();
            continue;
        }
        else return;
    }
}

void Parser::parameter()
{
    if (P_DEBUG) std::cout << "param" << '\n';

    SymTableEntry* entry = var_declaration(false, false);

    // curr_token is the typemark
    if (token() != TokenType::RS_IN
        && token() != TokenType::RS_OUT
        && token() != TokenType::RS_INOUT)
    {
        err_handler->reportError("Parameter passing type must be one of: IN or OUT or INOUT", curr_token.line);
    }
    entry->param_type = token(); // IN|OUT|INOUT
    advance();

    symtable_manager->add_param_to_proc(entry);
}

// need_alloc - variable needs to be allocated before using (defaults to true)
//  need_alloc==false for parameter variable declarations
SymTableEntry* Parser::var_declaration(bool is_global, bool need_alloc)
{
    if (P_DEBUG) std::cout << "var decl" << '\n';
    // This is the only place in grammar type mark occurs 
    //  so it doesn't need its own function
    TokenType typemark = token();
    advance();

    std::string id = require(TokenType::IDENTIFIER).val.string_value;
    SymTableEntry* entry = symtable_manager->resolve_symbol(id, false);
    if (entry != NULL && entry->sym_type != S_UNDEFINED)
    {
        std::ostringstream stream;
        stream << "Variable " << id << " may have already been defined the local or global scope.";
        err_handler->reportError(stream.str(), curr_token.line);
    }

    // The llvm type to allocate for this variable
    Type* allocation_type;

    // For initializing globals, if applicable
    Constant* global_init_constant;

    // entry sym_type is reduntant with entry's LLVM value
    switch (typemark)
    {
    case RS_STRING:
        entry->sym_type = S_STRING;
        // Strings are i8**
        allocation_type = Type::getInt8PtrTy(TheContext);
        if (is_global) 
            global_init_constant
                = ConstantInt::get(TheContext, APInt(8, 0));
        break;
    case RS_CHAR:
        entry->sym_type = S_CHAR;
        allocation_type = Type::getInt8Ty(TheContext);
        if (is_global) 
            global_init_constant 
                = ConstantInt::get(TheContext, APInt(8, 0));
        break;
    case RS_INTEGER:
        entry->sym_type = S_INTEGER;
        allocation_type = Type::getInt32Ty(TheContext);
        if (is_global) 
            global_init_constant 
                = ConstantInt::get(TheContext, APInt(32, 0));
        break;
    case RS_FLOAT:
        entry->sym_type = S_FLOAT;
        allocation_type = Type::getFloatTy(TheContext);
        if (is_global) 
            global_init_constant 
                = ConstantFP::get(TheContext, APFloat(0.));
        break;
    case RS_BOOL:
        entry->sym_type = S_BOOL;
        allocation_type = Type::getInt1Ty(TheContext);
        if (is_global) 
            global_init_constant 
                = ConstantInt::get(TheContext, APInt(1, 0));
        break;
    default:
        std::ostringstream stream;
        stream << "Unknown typemark: " << TokenTypeStrings[typemark];
        err_handler->reportError(stream.str(), curr_token.line);
        break;
    }

    // Only insert into global symbols if prefixed with RS_GLOBAL (is_global == true)
    if (is_global) symtable_manager->promote_to_global(id, entry);

    // Indexing
    if (token() == TokenType::L_BRACKET)
    {
        advance();

        int lower = lower_bound();
        require(TokenType::COLON);
        int upper = upper_bound();

        require(TokenType::R_BRACKET);

        entry->is_arr = true;
        entry->lower_b = lower;
        entry->upper_b = upper;

        int diff = upper - lower;
        entry->arr_size = diff;

        allocation_type = ArrayType::get(allocation_type, diff);
    }

    if (need_alloc)
    {
        if (is_global)
        {
            GlobalVariable* global = new GlobalVariable(*TheModule, 
                allocation_type, 
                false,
                GlobalValue::ExternalLinkage,
                global_init_constant,
                id,
                nullptr);

            global->setAlignment(16);
            // Initializer (all zero)
            global->setInitializer(ConstantAggregateZero::get(allocation_type));
            entry->value = global;
        }
        else
        {
            // Allocate space for this variable 
            entry->value 
                = Builder.CreateAlloca(allocation_type, nullptr, id);
        }
    }
    return entry;
}

int Parser::lower_bound()
{
    if (P_DEBUG) std::cout << "lower_bound" << '\n';
    // Minus allowed in spec now
    bool negative = false;
    if (token() == TokenType::MINUS) 
    {
        negative = true;
        advance();
    }
    int val = require(TokenType::INTEGER).val.int_value;
    if (negative) val = -val;
    return val;
}

int Parser::upper_bound()
{
    if (P_DEBUG) std::cout << "upper_bound" << '\n';
    // Minus allowed in spec now
    bool negative = false;
    if (token() == TokenType::MINUS) 
    {
        negative = true;
        advance();
    }
    int val = require(TokenType::INTEGER).val.int_value;
    if (negative) val = -val;
    return val;
}

bool Parser::statement()
{
    if (P_DEBUG) std::cout << "stmnt" << '\n';

    if (token() == TokenType::IDENTIFIER)
        identifier_statement();
    else if (token() == TokenType::RS_IF)
        if_statement();
    else if (token() == TokenType::RS_FOR)
        loop_statement();
    else if (token() == TokenType::RS_RETURN)
        return_statement();
    else return false;

    return true;
}

// Groups assignment and proc call statements, as both
//  start with an identifier
void Parser::identifier_statement()
{
    if (P_DEBUG) std::cout << "identifier stmnt" << '\n';
    // Advance to next token; returning the current token
    //  and retrieving the identifier value
    std::string identifier = advance().val.string_value;
    
    if (token() == TokenType::L_PAREN)
    {
        proc_call(identifier);
    }
    else
    {
        assignment_statement(identifier);
    }
}

void Parser::assignment_statement(std::string identifier)
{
    if (P_DEBUG) std::cout << "assignment stmnt" << '\n';

    // RS_OUT - we want to write to this variable
    SymTableEntry* entry = symtable_manager->resolve_symbol(identifier, true, RS_OUT); 

    // already have identifier; need to check for indexing first
    Value* idx = nullptr;
    if (token() == TokenType::L_BRACKET)
    {
        advance();
        Value* raw_idx = expression(Type::getInt32Ty(TheContext));
        require(TokenType::R_BRACKET);
        // Normalize index (subtract lower bound from index)
        idx = Builder.CreateSub(raw_idx, ConstantInt::get(TheContext, 
            APInt(32, entry->lower_b)));
    }

    Value* lhs = entry->value;

    // Handle pointer for the variable
    Type* lhs_stored_type = 
        cast<PointerType>(lhs->getType())->getElementType();

    if (entry->is_arr)
    {
        if (idx == nullptr)
        {
            // Assignment to entire array

            // expression must be an array of the same size as lhs
            //TODO
            /*
            lhs_stored_type = 
                ArrayType.get(lhs_stored_type, entry->arr_size);
                */
        }
        else
        {
            // Index with: [0, idx]
            const std::vector<Value*> GEPIdxs 
                {ConstantInt::get(TheContext, APInt(64, 0)), idx};
            lhs = Builder.CreateGEP(lhs, ArrayRef<Value*>(GEPIdxs));

            lhs_stored_type = 
                cast<ArrayType>(lhs_stored_type)->getArrayElementType();
        }
    }

    require(TokenType::ASSIGNMENT);

    Value* rhs = expression(lhs_stored_type);

    // Store rhs into lhs(ptr)
    Builder.CreateStore(rhs, lhs);
}

void Parser::proc_call(std::string identifier)
{
    if (P_DEBUG) std::cout << "proc call" << '\n';
    // already have identifier

    // Check symtable for the proc
    SymTableEntry* proc_entry = 
        symtable_manager->resolve_symbol(identifier, true); 

    if (proc_entry == NULL || proc_entry->sym_type != S_PROCEDURE)
    {
        std::ostringstream stream;
        stream << "Procedure " << identifier << " not defined\n";
        err_handler->reportError(stream.str(), curr_token.line);
        return;
    }


    std::vector<Value*> arg_list;
    require(TokenType::L_PAREN);
    if (token() != TokenType::R_PAREN)
        arg_list = argument_list(proc_entry);
    require(TokenType::R_PAREN);

    Builder.CreateCall(proc_entry->function, arg_list);
}

std::vector<Value*> Parser::argument_list(SymTableEntry* proc_entry)
{
    if (P_DEBUG) std::cout << "arg list" << '\n';

    std::vector<Value*> vec;

    Function* f = proc_entry->function;
    int parmidx = 0;
    for (auto& parm : f->args())
    {
        Value* param_val;

        // Parse an expression for a by ref type
        // Params need to be pointers for pass by ref (out or inout)
        if (PointerType* ptr_ty = dyn_cast<PointerType>(parm.getType()))
        {
            if (proc_entry->parameters[(parmidx)]->sym_type == S_STRING)
            {
                // It's a string. 
                if (proc_entry->parameters[(parmidx)]->param_type == RS_IN)
                {
                    // Pass by value. Expect i8*
                    param_val = expression(ptr_ty);
                }
                else
                {
                    // Pass by ref. Expect i8* but convert to i8**
                    Type* real_type = ptr_ty->getElementType();

                    Value* expr_result = expression(real_type);
                    if (auto *ptr = dyn_cast<LoadInst>(expr_result))
                    {
                        // val is a pointer type, just use the raw pointer (pass by ref)
                        param_val = ptr->getPointerOperand();
                    }
                    else
                    {
                        // We need to get
                        //  a pointer to the value the expression returns then we
                        //  can pass that into the function call
                        param_val = Builder.CreateAlloca(real_type);
                        // Store val into valptr
                        Builder.CreateStore(expr_result, param_val);
                    }
                }
            }
            else 
            {
                // The type is a pointer to a basic variable.
                // This case is for pass by ref 
                Type* real_type = ptr_ty->getElementType();

                Value* expr_result = expression(real_type);
                if (auto *ptr = dyn_cast<LoadInst>(expr_result))
                {
                    // val is a pointer type, just use the raw pointer (pass by ref)
                    param_val = ptr->getPointerOperand();
                }
                else
                {
                    // We need to get
                    //  a pointer to the value the expression returns then we
                    //  can pass that into the function call
                    param_val = Builder.CreateAlloca(real_type);
                    // Store val into valptr
                    Builder.CreateStore(expr_result, param_val);
                }
            }
        }
        else if (ArrayType* arr_ty = dyn_cast<ArrayType>(parm.getType()))
        {
            // It's an array
            Value* expr_result = expression(arr_ty);
            param_val = expr_result;
        }
        else 
        {
            // Parameter is an IN type (pass by value)
            param_val = expression(parm.getType());
        }

        // Expression should do type conversion.
        // If it's not the right type now, it probably can't be converted.
        if (parm.getType() != param_val->getType())
        {

            std::string str;
            raw_string_ostream rso(str);

            rso << "Procedure call paramater type doesn't match expected type. req'd: ";
            parm.getType()->print(rso, false);
            rso << " got: ";
            param_val->getType()->print(rso, false);

            rso.flush();

            err_handler->reportError(str, curr_token.line);
        }

        vec.push_back(param_val); 
        parmidx++;

        if (token() == TokenType::COMMA) 
        {
            advance();
            continue;
        }
        else break;
    }

    return vec;
}

void Parser::if_statement()
{
    if (P_DEBUG) std::cout << "if" << '\n';
    require(TokenType::RS_IF);

    require(TokenType::L_PAREN);
    Value* condition = expression(Type::getInt1Ty(TheContext));
    require(TokenType::R_PAREN);

    require(TokenType::RS_THEN);

    Function* TheFunction = symtable_manager->get_curr_proc_function();
    
    BasicBlock* then_block = BasicBlock::Create(TheContext, "then", TheFunction);
    BasicBlock* else_block = BasicBlock::Create(TheContext, "else");
    BasicBlock* after_block = BasicBlock::Create(TheContext, "after");

    Builder.CreateCondBr(condition, then_block, else_block);

    Builder.SetInsertPoint(then_block);

    bool first_stmnt = true;
    bool explicit_else = false;
    while (true)
    {
        bool valid = statement();
        // Make sure there is at least one valid statement
        if (first_stmnt && !valid)
        {
            err_handler->reportError("No statement in IF body", curr_token.line);
        }
        else require(TokenType::SEMICOLON);
        first_stmnt = false;

        if (token() == TokenType::RS_END) 
        {
            // Break from then to after block
            Builder.CreateBr(after_block);
            if (!explicit_else)
            {
                // Connect a dummy else block that just breaks to after block, 
                //  if there was no explicit else block defined
                TheFunction->getBasicBlockList().push_back(else_block);
                Builder.SetInsertPoint(else_block);
                Builder.CreateBr(after_block);
            }
            break;
        }
        if (token() == TokenType::RS_ELSE) 
        {
            explicit_else = true;
            // Break from then to after block
            Builder.CreateBr(after_block);
            // Connect else block
            TheFunction->getBasicBlockList().push_back(else_block);
            Builder.SetInsertPoint(else_block);
            advance();
            continue;
        }
    }

    TheFunction->getBasicBlockList().push_back(after_block);
    Builder.SetInsertPoint(after_block);
    
    require(TokenType::RS_END);
    require(TokenType::RS_IF);
}

void Parser::loop_statement()
{
    if (P_DEBUG) std::cout << "for" << '\n';
    require(TokenType::RS_FOR);

    require(TokenType::L_PAREN);
    require(TokenType::IDENTIFIER);
    assignment_statement(curr_token.val.string_value); 
    require(TokenType::SEMICOLON);

    Function* TheFunction = symtable_manager->get_curr_proc_function();
    
    BasicBlock* start_loop_block = BasicBlock::Create(TheContext, "start_loop", TheFunction);
    BasicBlock* loop_stmnts_block = BasicBlock::Create(TheContext, "loop_stmnts");
    BasicBlock* after_loop_block = BasicBlock::Create(TheContext, "after_loop");

    // Unconditionally break to the start block of the for
    Builder.CreateBr(start_loop_block); 

    // Begin for block(where expression is checked)
    TheFunction->getBasicBlockList().push_back(start_loop_block);
    Builder.SetInsertPoint(start_loop_block); 

    // Check expression
    Value* condition = expression(Type::getInt1Ty(TheContext));
    require(TokenType::R_PAREN);

    // Branch to statements or after loop denepding on condition
    Builder.CreateCondBr(condition, loop_stmnts_block, after_loop_block);

    // Begin for statements block
    TheFunction->getBasicBlockList().push_back(loop_stmnts_block);
    Builder.SetInsertPoint(loop_stmnts_block);

    // Consume and generate code for all inner for statements
    while (true)
    {
        statement();
        require(TokenType::SEMICOLON);
        if (token() == TokenType::RS_END) break;
    }

    // End of for statements; jmp to beginning of for (to check expr)
    Builder.CreateBr(start_loop_block);
    
    // Begin block after for
    TheFunction->getBasicBlockList().push_back(after_loop_block);
    Builder.SetInsertPoint(after_loop_block);

    require(TokenType::RS_END); // Just to be sure, also to advance the token
    require(TokenType::RS_FOR);
}

void Parser::return_statement()
{
    if (P_DEBUG) std::cout << "return" << '\n';
    require(TokenType::RS_RETURN);

    Builder.CreateRetVoid();

    // If there are any statements are after the return, they are unreachable
    BasicBlock* unreachable = BasicBlock::Create(TheContext, "unreachable");
    symtable_manager->get_curr_proc_function()->getBasicBlockList()
        .push_back(unreachable);
    Builder.SetInsertPoint(unreachable);
}

// hintType - the expected type (e.g. if this is an assignment)
//  used as a hint to expression on what type to convert to 
//  (if type conversion is needed)
Value* Parser::expression(Type* hintType)
{
    if (P_DEBUG) std::cout << "expr" << '\n';

    Value* retval;

    // arith_op is required and defined as:
    //  relation, arith_op_pr
    // expression_pr is completely optional; defined as:
    //  either & or |, arith_op, expression_pr

    if (token() == TokenType::RS_NOT)
    {
        // not <arith_op>
        advance();
        Value* val = arith_op(hintType);
        if (val->getType()->isIntegerTy(32))
        {
            retval = Builder.CreateXor(val, ConstantInt::get(TheContext, APInt(32, -1)));
        }
        if (val->getType()->isIntegerTy(1))
        {
            retval = Builder.CreateXor(val, ConstantInt::get(TheContext, APInt(1, 1)));
        }
        else
        {
            err_handler->reportError("Can only invert integers (bitwise) or bools (logical)", curr_token.line);
        }

        // Return becuase (not <arith_op>) is a complete expression
        retval = val;
    }
    else 
    {
        // Because arith_op is required to result in something, take its value
        //  and give it to expression_pr. expression_pr will return that value,
        //  either modified with its operation (& or |) and another arith_op, 
        //  (and optionally another expression_pr, and so on...)
        //  OR, expression_pr(val) will just return val unmodified 
        //  if there is no operator & or | as the first token.
        Value* val = arith_op(hintType); 
        retval = expression_pr(val, hintType);
    }

    // Type conversion to expected type before returning from expression.
    if (hintType != retval->getType())
    {
        retval = convert_type(retval, hintType);
    }

    return retval;
}

// lhs - left hand side of this operation. 
Value* Parser::expression_pr(Value* lhs, Type* hintType)
{
    if (P_DEBUG) std::cout << "expr prime" << '\n';


    if (token() == TokenType::AND
        || token() == TokenType::OR)
    {
        TokenType op = advance().type;

        Value* rhs = arith_op(hintType);

        // If one is a bool and one an int, convert
        if (lhs->getType() == Type::getInt1Ty(TheContext) 
            && rhs->getType() == Type::getInt32Ty(TheContext))
        {
            if (hintType == Type::getInt1Ty(TheContext))
                rhs = convert_type(rhs, Type::getInt1Ty(TheContext));
            else 
                lhs = convert_type(lhs, Type::getInt32Ty(TheContext));
        }
        else if (lhs->getType() == Type::getInt32Ty(TheContext) 
            && rhs->getType() == Type::getInt1Ty(TheContext))
        {
            if (hintType == Type::getInt1Ty(TheContext))
                lhs = convert_type(lhs, Type::getInt1Ty(TheContext));
            else 
                rhs = convert_type(rhs, Type::getInt32Ty(TheContext));
        }
        else if (lhs->getType() != rhs->getType()
                    && lhs->getType() != Type::getInt1Ty(TheContext) 
                    && lhs->getType() != Type::getInt32Ty(TheContext))
        {
            // Types aren't the same or aren't both bool/int
            err_handler->reportError("Bitwise or boolean operations are only defined on bool and integer types", curr_token.line);
        }

        Value* result;
        if (op == TokenType::AND)
            result = Builder.CreateAnd(lhs, rhs);
        else
            result = Builder.CreateOr(lhs, rhs);

        return expression_pr(result, hintType);
    }

    // No operation performed; return lhs unmodified.
    else return lhs;
}

Value* Parser::arith_op(Type* hintType)
{
    if (P_DEBUG) std::cout << "arith op" << '\n';

    Value* val = relation(hintType);
    return arith_op_pr(val, hintType);
}

Value* Parser::arith_op_pr(Value* lhs, Type* hintType)
{
    if (P_DEBUG) std::cout << "arith op pr" << '\n';

    if (token() == TokenType::PLUS || token() == TokenType::MINUS)
    {
        // Advance and save current token's operator.
        TokenType op = advance().type;

        Value* rhs = relation(hintType); 

        // Type conversion

        // If one is int and one is float, 
        //  convert all to float to get the most precision.
        // Any other types can't be used here.
        if (lhs->getType() == Type::getInt32Ty(TheContext) 
            && rhs->getType() == Type::getFloatTy(TheContext))
        {
            // Convert lhs to float
            lhs = convert_type(lhs, Type::getFloatTy(TheContext));
        }
        else if (lhs->getType() == Type::getFloatTy(TheContext) 
            && rhs->getType() == Type::getInt32Ty(TheContext))
        {
            // Convert rhs to float
            rhs = convert_type(rhs, Type::getFloatTy(TheContext));
        }
        else if (lhs->getType() != rhs->getType() 
                    && lhs->getType() != Type::getFloatTy(TheContext)
                    && lhs->getType() != Type::getInt32Ty(TheContext))
        {
            // Types aren't the same or aren't both float/int
            err_handler->reportError("Arithmetic operations are only defined on float and integer types", curr_token.line);
            // TODO: Return?
        }

        Value* result;
        if (op == TokenType::PLUS)
        {
            if (lhs->getType()->isFloatTy())
                result = Builder.CreateFAdd(lhs, rhs);
            else
                result = Builder.CreateAdd(lhs, rhs);
        }
        else
        {
            if (lhs->getType()->isFloatTy())
                result = Builder.CreateFSub(lhs, rhs);
            else
                result = Builder.CreateSub(lhs, rhs);
        }

        return arith_op_pr(result, hintType);
    }
    else return lhs;
}

Value* Parser::relation(Type* hintType)
{
    if (P_DEBUG) std::cout << "relation" << '\n';

    Value* val = term(hintType);
    return relation_pr(val, hintType);
}

Value* Parser::relation_pr(Value* lhs, Type* hintType)
{
    if (P_DEBUG) std::cout << "relation pr" << '\n';

    if ((token() == TokenType::LT)
        || (token() == TokenType::GT)
        || (token() == TokenType::LT_EQ)
        || (token() == TokenType::GT_EQ)
        || (token() == TokenType::EQUALS)
        || (token() == TokenType::NOTEQUAL))
    {
        TokenType op = advance().type;
        Value* rhs = term(hintType);

        // Type conversion
        if (lhs->getType() != rhs->getType())
        {
            if (lhs->getType() == Type::getFloatTy(TheContext) 
                && rhs->getType() == Type::getInt32Ty(TheContext))
            {
                // Always convert up to float to avoid losing information
                convert_type(rhs, Type::getFloatTy(TheContext));
            }
            else if (lhs->getType() == Type::getInt32Ty(TheContext) 
                && rhs->getType() == Type::getFloatTy(TheContext))
            {
                // Always convert up to float to avoid losing information
                convert_type(lhs, Type::getFloatTy(TheContext));
            }
            else if (lhs->getType() == Type::getInt1Ty(TheContext)
                && rhs->getType() == Type::getInt32Ty(TheContext))
            {
                // Prefer conversion to hint type if possible to simplify later
                if (hintType == Type::getInt1Ty(TheContext)) 
                    convert_type(rhs, Type::getInt1Ty(TheContext));
                else 
                    convert_type(lhs, Type::getInt32Ty(TheContext));
            }
            else if (lhs->getType() == Type::getInt32Ty(TheContext)     
                && rhs->getType() == Type::getInt1Ty(TheContext))
            {
                // Prefer conversion to hint type if possible to simplify later
                if (hintType == Type::getInt1Ty(TheContext)) 
                    convert_type(lhs, Type::getInt1Ty(TheContext));
                else 
                    convert_type(rhs, Type::getInt32Ty(TheContext));
            }
            else
            {

                err_handler->reportError("Incompatible types for relational operators", 
                    curr_token.line);
            }
        }

        Value* result;
        switch (op)
        {
            case LT:
                if (lhs->getType()->isFloatTy())
                    result = Builder.CreateFCmpOLE(lhs, rhs);
                else
                    result = Builder.CreateICmpSLT(lhs, rhs);
                break;
            case GT:
                if (lhs->getType()->isFloatTy())
                    result = Builder.CreateFCmpOGT(lhs, rhs);
                else
                    result = Builder.CreateICmpSGT(lhs, rhs);
                break;
            case LT_EQ:
                if (lhs->getType()->isFloatTy())
                    result = Builder.CreateFCmpOLE(lhs, rhs);
                else
                    result = Builder.CreateICmpSLE(lhs, rhs);
                break;
            case GT_EQ:
                if (lhs->getType()->isFloatTy())
                    result = Builder.CreateFCmpOGE(lhs, rhs);
                else
                    result = Builder.CreateICmpSGE(lhs, rhs);
                break;
            case EQUALS:
                if (lhs->getType()->isFloatTy())
                    result = Builder.CreateFCmpOEQ(lhs, rhs);
                else
                    result = Builder.CreateICmpEQ(lhs, rhs);
                break;
            case NOTEQUAL:
                if (lhs->getType()->isFloatTy())
                    result = Builder.CreateFCmpONE(lhs, rhs);
                else
                    result = Builder.CreateICmpNE(lhs, rhs);
                break;
            default:
                // This shouldn't happen
                result = nullptr;
                break;
        }

        return relation_pr(result, hintType);
    }
    else return lhs;
}

Value* Parser::term(Type* hintType)
{
    if (P_DEBUG) std::cout << "term" << '\n';

    Value* val = factor(hintType);
    return term_pr(val, hintType);
}

// Multiplication / Division 
Value* Parser::term_pr(Value* lhs, Type* hintType)
{
    if (P_DEBUG) std::cout << "term pr" << '\n';


    if (token() == TokenType::MULTIPLICATION 
        || token() == TokenType::DIVISION)
    {
        TokenType op = advance().type;
        Value* rhs = factor(hintType);

        // Type checking
        if (lhs->getType() == Type::getInt32Ty(TheContext)
            && rhs->getType() == Type::getFloatTy(TheContext))
        {
            // Convert lhs to float
            lhs = convert_type(lhs, Type::getFloatTy(TheContext));
        }
        else if (lhs->getType() == Type::getFloatTy(TheContext) 
            && rhs->getType() == Type::getInt32Ty(TheContext))
        {
            // Convert rhs to float
            rhs = convert_type(rhs, Type::getFloatTy(TheContext));
        }
        else if (lhs->getType() != rhs->getType() 
                    && lhs->getType() != Type::getFloatTy(TheContext)
                    && lhs->getType() != Type::getInt32Ty(TheContext))
        {
            // Types aren't the same or aren't both float/int
            err_handler->reportError("Term operations (multiplication and division) are only defined on float and integer types.", curr_token.line);
        }

        Value* result;

        if (op == TokenType::MULTIPLICATION)
        {
            if (lhs->getType()->isFloatTy())
                result = Builder.CreateFMul(lhs, rhs);
            else
                result = Builder.CreateMul(lhs, rhs);
        }
        else
        {
            if (lhs->getType()->isFloatTy())
                result = Builder.CreateFDiv(lhs, rhs);
            else
                result = Builder.CreateSDiv(lhs, rhs);
        }

        return term_pr(result, hintType);
    }
    else return lhs;
}

Value* Parser::factor(Type* hintType)
{
    if (P_DEBUG) std::cout << "factor" << '\n';
    Value* retval = nullptr;

    // Token is one of:
    //  (expression), [-] name, [-] float|integer, string, char, bool 
    if (token() == TokenType::L_PAREN)
    {
        advance();
        retval = expression(hintType);
        require(TokenType::R_PAREN);
    }
    else if (token() == TokenType::MINUS)
    {
        advance();

        if (token() == TokenType::INTEGER)
        {
            MyValue mval = advance().val;
            APInt negated_int = APInt(32, mval.int_value);
            negated_int.negate();
            return ConstantInt::get(TheContext, negated_int);
        }
        else if (token() == TokenType::FLOAT) 
        {
            MyValue mval = advance().val;
            APFloat negated_float = APFloat(mval.float_value);
            negated_float.changeSign();
            return ConstantFP::get(TheContext, negated_float);
        }
        else if (token() == TokenType::IDENTIFIER) 
        {
            Value* nameVal = name(hintType);
            if (nameVal->getType() == Type::getInt32Ty(TheContext))
                return Builder.CreateNeg(nameVal);
            else if (nameVal->getType() == Type::getFloatTy(TheContext))
                return Builder.CreateFNeg(nameVal);
        }

        // One of the paths above should have returned
        // Being here is an error
        std::ostringstream stream;
        stream << "Bad token following negative sign: " << TokenTypeStrings[token()];
        err_handler->reportError(stream.str(), curr_token.line);
        advance();
    }
    else if (token() == TokenType::IDENTIFIER)
    {
        retval = name(hintType);
    }
    else if (token() == STRING)
    {
        MyValue mval = advance().val;
        int len = mval.string_value.length() + 1; // +1 for \0
        GlobalVariable* string = new GlobalVariable(*TheModule, 
            ArrayType::get(Type::getInt8Ty(TheContext), len), 
            true,
            GlobalValue::ExternalLinkage,
            0);
        Constant *string_arr = ConstantDataArray::getString(TheContext, 
            mval.string_value, true);
        string->setInitializer(string_arr);

        retval = string;
    }
    else if (token() == CHAR)
    {
        MyValue mval = advance().val;
        retval = ConstantInt::get(TheContext, APInt(8, mval.char_value));
    }
    else if (token() == INTEGER)
    {
        MyValue mval = advance().val;
        retval = ConstantInt::get(TheContext, APInt(32, mval.int_value));
    }
    else if (token() == FLOAT)
    {
        MyValue mval = advance().val;
        retval = ConstantFP::get(TheContext, APFloat(mval.float_value));
    }
    else if (token() == RS_TRUE || token() == RS_FALSE)
    {
        MyValue mval = advance().val;
        retval = ConstantInt::get(TheContext, APInt(1, mval.int_value));
    }
    else
    {
        std::ostringstream stream;
        stream << "Invalid token type in factor: " << TokenTypeStrings[token()];
        // Consume token and get line number 
        err_handler->reportError(stream.str(), advance().line);
    }
    
    return retval;
}

Value* Parser::name(Type* hintType)
{
    if (P_DEBUG) std::cout << "name" << '\n';

    std::string id = require(TokenType::IDENTIFIER).val.string_value;

    // RS_IN - we expect to be able to read this variable's value
    SymTableEntry* entry = symtable_manager->resolve_symbol(id, true, RS_IN);

    Value* val_to_load;
    val_to_load = entry->value;

    if (token() == TokenType::L_BRACKET)
    {
        advance();
        Value* idxval = expression(Type::getInt32Ty(TheContext));
        require(TokenType::R_BRACKET);
        // Normalize idxval (subtract the lower bound from the index)
        Value* normalized_idx 
            = Builder.CreateSub(idxval, 
                ConstantInt::get(TheContext, APInt(32, entry->lower_b)));

        // Index var (use GEP, which can then be loaded same as other vars)
        std::vector<Value*> GEPIdxs;
        GEPIdxs.push_back(ConstantInt::get(TheContext, APInt(64, 0)));
        //GEPIdxs.push_back(ConstantInt::get(TheContext, APInt(64, 0)));
        GEPIdxs.push_back(normalized_idx);

        TheModule->print(llvm::errs(), NULL);
        val_to_load = Builder.CreateGEP(val_to_load, ArrayRef<Value*>(GEPIdxs));
        // THe fuck is this
        /*
        else if (isa<ArrayType>(val_to_load->getType()))
        {
            std::vector<unsigned> Idxs;
            Idxs.push_back(normalized_idx);
            Builder.CreateExtractValue(val_to_load, ArrayRef<unsigned>(Idxs));
        }
        */
    }

    Value* retval;
    retval = Builder.CreateLoad(val_to_load, id);

    return retval;
}

