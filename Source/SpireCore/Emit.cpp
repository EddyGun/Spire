// Emit.cpp
#include "Emit.h"

#include "Syntax.h"

#include <assert.h>

#ifdef _WIN32
#include <d3dcompiler.h>
#pragma warning(disable:4996)
#endif

namespace Spire { namespace Compiler {

struct EmitContext
{
    StringBuilder sb;
};

//

static void EmitDecl(EmitContext* context, RefPtr<Decl> decl);
static void EmitDecl(EmitContext* context, RefPtr<DeclBase> declBase);
static void EmitType(EmitContext* context, RefPtr<ExpressionType> type, String const& name);
static void EmitType(EmitContext* context, RefPtr<ExpressionType> type);
static void EmitExpr(EmitContext* context, RefPtr<ExpressionSyntaxNode> expr);
static void EmitStmt(EmitContext* context, RefPtr<StatementSyntaxNode> stmt);
static void EmitDeclRef(EmitContext* context, DeclRef declRef);

// Low-level emit logic

static void Emit(EmitContext* context, char const* textBegin, char const* textEnd)
{
    context->sb.Append(textBegin, int(textEnd - textBegin));
}

static void Emit(EmitContext* context, char const* text)
{
    Emit(context, text, text + strlen(text));
}

static void Emit(EmitContext* context, String const& text)
{
    Emit(context, text.begin(), text.end());
}

static void Emit(EmitContext* context, int value)
{
    char buffer[16];
    sprintf(buffer, "%d", value);
    Emit(context, buffer);
}

static void Emit(EmitContext* context, double value)
{
    // TODO(tfoley): need to print things in a way that can round-trip
    char buffer[128];
    sprintf(buffer, "%f", value);
    Emit(context, buffer);
}

// Expressions

// Determine if an expression should not be emitted when it is the base of
// a member reference expression.
static bool IsBaseExpressionImplicit(EmitContext* /*context*/, RefPtr<ExpressionSyntaxNode> expr)
{
    // HACK(tfoley): For now, anything with a constant-buffer type should be
    // left implicit.

    // Look through any dereferencing that took place
    RefPtr<ExpressionSyntaxNode> e = expr;
    while (auto derefExpr = e.As<DerefExpr>())
    {
        e = derefExpr->base;
    }
    // Is the expression referencing a constant buffer?
    if (auto cbufferType = e->Type->As<ConstantBufferType>())
    {
        return true;
    }

    return false;
}

enum
{
    kPrecedence_None,
    kPrecedence_Comma,

    kPrecedence_Assign,
    kPrecedence_AddAssign = kPrecedence_Assign,
    kPrecedence_SubAssign = kPrecedence_Assign,
    kPrecedence_MulAssign = kPrecedence_Assign,
    kPrecedence_DivAssign = kPrecedence_Assign,
    kPrecedence_ModAssign = kPrecedence_Assign,
    kPrecedence_LshAssign = kPrecedence_Assign,
    kPrecedence_RshAssign = kPrecedence_Assign,
    kPrecedence_OrAssign = kPrecedence_Assign,
    kPrecedence_AndAssign = kPrecedence_Assign,
    kPrecedence_XorAssign = kPrecedence_Assign,

    kPrecedence_General = kPrecedence_Assign,

    kPrecedence_Conditional, // "ternary"
    kPrecedence_Or,
    kPrecedence_And,
    kPrecedence_BitOr,
    kPrecedence_BitXor,
    kPrecedence_BitAnd,

    kPrecedence_Eql,
    kPrecedence_Neq = kPrecedence_Eql,

    kPrecedence_Less,
    kPrecedence_Greater = kPrecedence_Less,
    kPrecedence_Leq = kPrecedence_Less,
    kPrecedence_Geq = kPrecedence_Less,

    kPrecedence_Lsh,
    kPrecedence_Rsh = kPrecedence_Lsh,

    kPrecedence_Add,
    kPrecedence_Sub = kPrecedence_Add,

    kPrecedence_Mul,
    kPrecedence_Div = kPrecedence_Mul,
    kPrecedence_Mod = kPrecedence_Mul,

    kPrecedence_Prefix,
    kPrecedence_Postifx,
    kPrecedence_Atomic = kPrecedence_Postifx
};

static void EmitExprWithPrecedence(EmitContext* context, RefPtr<ExpressionSyntaxNode> expr, int outerPrec);

static void EmitPostfixExpr(EmitContext* context, RefPtr<ExpressionSyntaxNode> expr)
{
    EmitExprWithPrecedence(context, expr, kPrecedence_Postifx);
}

static void EmitExpr(EmitContext* context, RefPtr<ExpressionSyntaxNode> expr)
{
    EmitExprWithPrecedence(context, expr, kPrecedence_General);
}

static bool MaybeEmitParens(EmitContext* context, int outerPrec, int prec)
{
    if (prec <= outerPrec)
    {
        Emit(context, "(");
        return true;
    }
    return false;
}

static void EmitBinExpr(EmitContext* context, int outerPrec, int prec, char const* op, RefPtr<OperatorExpressionSyntaxNode> binExpr)
{
    bool needsClose = MaybeEmitParens(context, outerPrec, prec);
    EmitExprWithPrecedence(context, binExpr->Arguments[0], prec);
    Emit(context, " ");
    Emit(context, op);
    Emit(context, " ");
    EmitExprWithPrecedence(context, binExpr->Arguments[1], prec);
    if (needsClose)
    {
        Emit(context, ")");
    }
}

static void EmitUnaryExpr(
    EmitContext* context,
    int outerPrec,
    int prec,
    char const* preOp,
    char const* postOp,
    RefPtr<OperatorExpressionSyntaxNode> binExpr)
{
    bool needsClose = MaybeEmitParens(context, outerPrec, prec);
    Emit(context, preOp);
    EmitExprWithPrecedence(context, binExpr->Arguments[0], prec);
    Emit(context, postOp);
    if (needsClose)
    {
        Emit(context, ")");
    }
}

static void EmitExprWithPrecedence(EmitContext* context, RefPtr<ExpressionSyntaxNode> expr, int outerPrec)
{
    bool needClose = false;
    if (auto binExpr = expr.As<OperatorExpressionSyntaxNode>())
    {
        switch (binExpr->Operator)
        {
#define CASE(NAME, OP) case Operator::NAME: EmitBinExpr(context, outerPrec, kPrecedence_##NAME, #OP, binExpr); break
            CASE(Mul, *);
            CASE(Div, / );
            CASE(Mod, %);
            CASE(Add, +);
            CASE(Sub, -);
            CASE(Lsh, << );
            CASE(Rsh, >> );
            CASE(Eql, == );
            CASE(Neq, != );
            CASE(Greater, >);
            CASE(Less, <);
            CASE(Geq, >= );
            CASE(Leq, <= );
            CASE(BitAnd, &);
            CASE(BitXor, ^);
            CASE(BitOr, | );
            CASE(And, &&);
            CASE(Or, || );
            CASE(Assign, =);
            CASE(AddAssign, +=);
            CASE(SubAssign, -=);
            CASE(MulAssign, *=);
            CASE(DivAssign, /=);
            CASE(ModAssign, %=);
            CASE(LshAssign, <<=);
            CASE(RshAssign, >>=);
            CASE(OrAssign, |=);
            CASE(AndAssign, &=);
            CASE(XorAssign, ^=);
#undef CASE
        case Operator::Sequence: EmitBinExpr(context, outerPrec, kPrecedence_Comma, ",", binExpr); break;
#define PREFIX(NAME, OP) case Operator::NAME: EmitUnaryExpr(context, outerPrec, kPrecedence_Prefix, #OP, "", binExpr); break
#define POSTFIX(NAME, OP) case Operator::NAME: EmitUnaryExpr(context, outerPrec, kPrecedence_Postfix, "", #OP, binExpr); break

            PREFIX(Neg, -);
            PREFIX(Not, !);
            PREFIX(BitNot, ~);
            PREFIX(PreInc, ++);
            PREFIX(PreDec, --);
            PREFIX(PostInc, ++);
            PREFIX(PostDec, --);
#undef PREFIX
#undef POSTFIX
        default:
            assert(!"unreachable");
            break;
        }
    }
    else if (auto appExpr = expr.As<InvokeExpressionSyntaxNode>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Postifx);

        auto funcExpr = appExpr->FunctionExpr;
        if (auto funcDeclRefExpr = funcExpr.As<DeclRefExpr>())
        {
            auto declRef = funcDeclRefExpr->declRef;
            if (auto ctorDeclRef = declRef.As<ConstructorDeclRef>())
            {
                // We really want to emit a reference to the type begin constructed
                EmitType(context, expr->Type);
            }
            else
            {
                // default case: just emit the decl ref
                EmitExpr(context, funcExpr);
            }
        }
        else
        {
            // default case: just emit the expression
            EmitPostfixExpr(context, funcExpr);
        }

        Emit(context, "(");
        int argCount = appExpr->Arguments.Count();
        for (int aa = 0; aa < argCount; ++aa)
        {
            if (aa != 0) Emit(context, ", ");
            EmitExpr(context, appExpr->Arguments[aa]);
        }
        Emit(context, ")");
    }
    else if (auto memberExpr = expr.As<MemberExpressionSyntaxNode>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Postifx);

        // TODO(tfoley): figure out a good way to reference
        // declarations that might be generic and/or might
        // not be generated as lexically nested declarations...

        // TODO(tfoley): also, probably need to special case
        // this for places where we are using a built-in...

        auto base = memberExpr->BaseExpression;
        if (IsBaseExpressionImplicit(context, base))
        {
            // don't emit the base expression
        }
        else
        {
            EmitExprWithPrecedence(context, memberExpr->BaseExpression, kPrecedence_Postifx);
            Emit(context, ".");
        }

        Emit(context, memberExpr->declRef.GetName());
    }
    else if (auto swizExpr = expr.As<SwizzleExpr>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Postifx);

        EmitExprWithPrecedence(context, swizExpr->base, kPrecedence_Postifx);
        Emit(context, ".");
        static const char* kComponentNames[] = { "x", "y", "z", "w" };
        int elementCount = swizExpr->elementCount;
        for (int ee = 0; ee < elementCount; ++ee)
        {
            Emit(context, kComponentNames[swizExpr->elementIndices[ee]]);
        }
    }
    else if (auto indexExpr = expr.As<IndexExpressionSyntaxNode>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Postifx);

        EmitExprWithPrecedence(context, indexExpr->BaseExpression, kPrecedence_Postifx);
        Emit(context, "[");
        EmitExpr(context, indexExpr->IndexExpression);
        Emit(context, "]");
    }
    else if (auto varExpr = expr.As<VarExpressionSyntaxNode>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Atomic);

        EmitDeclRef(context, varExpr->declRef);
    }
    else if (auto derefExpr = expr.As<DerefExpr>())
    {
        // TODO(tfoley): dereference shouldn't always be implicit
        EmitExprWithPrecedence(context, derefExpr->base, outerPrec);
    }
    else if (auto litExpr = expr.As<ConstantExpressionSyntaxNode>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Atomic);

        switch (litExpr->ConstType)
        {
        case ConstantExpressionSyntaxNode::ConstantType::Int:
            Emit(context, litExpr->IntValue);
            break;
        case ConstantExpressionSyntaxNode::ConstantType::Float:
            Emit(context, litExpr->FloatValue);
            break;
        case ConstantExpressionSyntaxNode::ConstantType::Bool:
            Emit(context, litExpr->IntValue ? "true" : "false");
            break;
        default:
            assert(!"unreachable");
            break;
        }
    }
    else if (auto castExpr = expr.As<TypeCastExpressionSyntaxNode>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Prefix);

        Emit(context, "(");
        EmitType(context, castExpr->Type);
        Emit(context, ") ");
        EmitExpr(context, castExpr->Expression);
    }
    else if (auto selectExpr = expr.As<SelectExpressionSyntaxNode>())
    {
        needClose = MaybeEmitParens(context, outerPrec, kPrecedence_Conditional);

        EmitExprWithPrecedence(context, selectExpr->SelectorExpr, kPrecedence_Conditional);
        Emit(context, " ? ");
        EmitExprWithPrecedence(context, selectExpr->Expr0, kPrecedence_Conditional);
        Emit(context, " : ");
        EmitExprWithPrecedence(context, selectExpr->Expr1, kPrecedence_Conditional);
    }
    else
    {
        throw "unimplemented";
    }
    if (needClose)
    {
        Emit(context, ")");
    }
}

// Types

// Extract the actual value of a compile-time integer
static int GetIntVal(RefPtr<IntVal> val)
{
    if (auto constantVal = val.As<ConstantIntVal>())
        return constantVal->value;
    assert(!"unexpected");
    return 0;
}

void Emit(EmitContext* context, RefPtr<IntVal> val)
{
    Emit(context, GetIntVal(val));
}

// represents a declarator for use in emitting types
struct EDeclarator
{
    enum class Flavor
    {
        Name,
        Array,
    };
    Flavor flavor;
    EDeclarator* next = nullptr;

    // Used for `Flavor::Name`
    String name;

    // Used for `Flavor::Array`
    int elementCount;
};

static void EmitDeclarator(EmitContext* context, EDeclarator* declarator)
{
    if (!declarator) return;

    Emit(context, " ");

    switch (declarator->flavor)
    {
    case EDeclarator::Flavor::Name:
        Emit(context, declarator->name);
        break;

    case EDeclarator::Flavor::Array:
        EmitDeclarator(context, declarator->next);
        Emit(context, "[");
        if(auto elementCount = declarator->elementCount)
        {
            Emit(context, elementCount);
        }
        Emit(context, "]");
        break;

    default:
        assert(!"unreachable");
        break;
    }
}

static void EmitType(EmitContext* context, RefPtr<ExpressionType> type, EDeclarator* declarator)
{
    if (auto basicType = type->As<BasicExpressionType>())
    {
        switch (basicType->BaseType)
        {
        case BaseType::Void:	Emit(context, "void");		break;
        case BaseType::Int:		Emit(context, "int");		break;
        case BaseType::Float:	Emit(context, "float");		break;
        case BaseType::UInt:	Emit(context, "uint");		break;
        case BaseType::Bool:	Emit(context, "bool");		break;
        case BaseType::Error:	Emit(context, "<error>");	break;
        default:
            assert(!"unreachable");
            break;
        }

        EmitDeclarator(context, declarator);
        return;
    }
    else if (auto vecType = type->As<VectorExpressionType>())
    {
        // TODO(tfoley): should really emit these with sugar
        Emit(context, "vector<");
        EmitType(context, vecType->elementType);
        Emit(context, ",");
        Emit(context, vecType->elementCount);
        Emit(context, "> ");

        EmitDeclarator(context, declarator);
        return;
    }
    else if (auto matType = type->As<MatrixExpressionType>())
    {
        // TODO(tfoley): should really emit these with sugar
        Emit(context, "matrix<");
        EmitType(context, matType->elementType);
        Emit(context, ",");
        Emit(context, matType->rowCount);
        Emit(context, ",");
        Emit(context, matType->colCount);
        Emit(context, "> ");

        EmitDeclarator(context, declarator);
        return;
    }
    else if (auto texType = type->As<TextureType>())
    {
        switch (texType->GetBaseShape())
        {
        case TextureType::Shape1D:		Emit(context, "Texture1D");		break;
        case TextureType::Shape2D:		Emit(context, "Texture2D");		break;
        case TextureType::Shape3D:		Emit(context, "Texture3D");		break;
        case TextureType::ShapeCube:	Emit(context, "TextureCube");	break;
        default:
            assert(!"unreachable");
            break;
        }

        if (texType->isMultisample())
        {
            Emit(context, "MS");
        }
        if (texType->isArray())
        {
            Emit(context, "Array");
        }
        Emit(context, "<");
        EmitType(context, texType->elementType);
        Emit(context, "> ");

        EmitDeclarator(context, declarator);
        return;
    }
    else if (auto samplerStateType = type->As<SamplerStateType>())
    {
        switch (samplerStateType->flavor)
        {
        case SamplerStateType::Flavor::SamplerState:			Emit(context, "SamplerState");				break;
        case SamplerStateType::Flavor::SamplerComparisonState:	Emit(context, "SamplerComparisonState");	break;
        default:
            assert(!"unreachable");
            break;
        }

        EmitDeclarator(context, declarator);
        return;
    }
    else if (auto declRefType = type->As<DeclRefType>())
    {
        EmitDeclRef(context,  declRefType->declRef);

        EmitDeclarator(context, declarator);
        return;
    }
    else if( auto arrayType = type->As<ArrayExpressionType>() )
    {
        EDeclarator arrayDeclarator;
        arrayDeclarator.next = declarator;
        arrayDeclarator.flavor = EDeclarator::Flavor::Array;
        arrayDeclarator.elementCount = GetIntVal(arrayType->ArrayLength);

        EmitType(context, arrayType->BaseType, &arrayDeclarator);
        return;
    }

    throw "unimplemented";
}

static void EmitType(EmitContext* context, RefPtr<ExpressionType> type, String const& name)
{
    EDeclarator nameDeclarator;
    nameDeclarator.flavor = EDeclarator::Flavor::Name;
    nameDeclarator.name = name;
    EmitType(context, type, &nameDeclarator);
}

static void EmitType(EmitContext* context, RefPtr<ExpressionType> type)
{
    EmitType(context, type, nullptr);
}

// Statements

// Emit a statement as a `{}`-enclosed block statement, but avoid adding redundant
// curly braces if the statement is itself a block statement.
static void EmitBlockStmt(EmitContext* context, RefPtr<StatementSyntaxNode> stmt)
{
    // TODO(tfoley): support indenting
    Emit(context, "{\n");
    if( auto blockStmt = stmt.As<BlockStatementSyntaxNode>() )
    {
        for (auto s : blockStmt->Statements)
        {
            EmitStmt(context, s);
        }
    }
    else
    {
        EmitStmt(context, stmt);
    }
    Emit(context, "}\n");
}

static void EmitLoopAttributes(EmitContext* context, RefPtr<StatementSyntaxNode> decl)
{
    // TODO(tfoley): There really ought to be a semantic checking step for attributes,
    // that turns abstract syntax into a concrete hierarchy of attribute types (e.g.,
    // a specific `LoopModifier` or `UnrollModifier`).

    for(auto attr : decl->GetModifiersOfType<SimpleAttribute>())
    {
        if(attr->Key == "loop")
        {
            Emit(context, "[loop]");
        }
        else if(attr->Key == "unroll")
        {
            Emit(context, "[unroll]");
        }
    }
}

static void EmitStmt(EmitContext* context, RefPtr<StatementSyntaxNode> stmt)
{
    if (auto blockStmt = stmt.As<BlockStatementSyntaxNode>())
    {
        EmitBlockStmt(context, blockStmt);
        return;
    }
    else if (auto exprStmt = stmt.As<ExpressionStatementSyntaxNode>())
    {
        EmitExpr(context, exprStmt->Expression);
        Emit(context, ";\n");
        return;
    }
    else if (auto returnStmt = stmt.As<ReturnStatementSyntaxNode>())
    {
        Emit(context, "return");
        if (auto expr = returnStmt->Expression)
        {
            Emit(context, " ");
            EmitExpr(context, expr);
        }
        Emit(context, ";\n");
        return;
    }
    else if (auto declStmt = stmt.As<VarDeclrStatementSyntaxNode>())
    {
        EmitDecl(context, declStmt->decl);
        return;
    }
    else if (auto ifStmt = stmt.As<IfStatementSyntaxNode>())
    {
        Emit(context, "if(");
        EmitExpr(context, ifStmt->Predicate);
        Emit(context, ")\n");
        EmitBlockStmt(context, ifStmt->PositiveStatement);
        if(auto elseStmt = ifStmt->NegativeStatement)
        {
            Emit(context, "\nelse\n");
            EmitBlockStmt(context, elseStmt);
        }
        return;
    }
    else if (auto forStmt = stmt.As<ForStatementSyntaxNode>())
    {
        EmitLoopAttributes(context, forStmt);

        Emit(context, "for(");
        if (auto initStmt = forStmt->InitialStatement)
        {
            EmitStmt(context, initStmt);
        }
        else
        {
            Emit(context, ";");
        }
        if (auto testExp = forStmt->PredicateExpression)
        {
            EmitExpr(context, testExp);
        }
        Emit(context, ";");
        if (auto incrExpr = forStmt->SideEffectExpression)
        {
            EmitExpr(context, incrExpr);
        }
        Emit(context, ")\n");
        EmitBlockStmt(context, forStmt->Statement);
        return;
    }
    else if (auto discardStmt = stmt.As<DiscardStatementSyntaxNode>())
    {
        Emit(context, "discard;\n");
        return;
    }
    else if (auto emptyStmt = stmt.As<EmptyStatementSyntaxNode>())
    {
        return;
    }
    else if (auto switchStmt = stmt.As<SwitchStmt>())
    {
        Emit(context, "switch(");
        EmitExpr(context, switchStmt->condition);
        Emit(context, ")\n");
        EmitBlockStmt(context, switchStmt->body);
        return;
    }
    else if (auto caseStmt = stmt.As<CaseStmt>())
    {
        Emit(context, "case ");
        EmitExpr(context, caseStmt->expr);
        Emit(context, ":\n");
        return;
    }
    else if (auto defaultStmt = stmt.As<DefaultStmt>())
    {
        Emit(context, "default:{}\n");
        return;
    }
    else if (auto breakStmt = stmt.As<BreakStatementSyntaxNode>())
    {
        Emit(context, "break;\n");
        return;
    }
    else if (auto continueStmt = stmt.As<ContinueStatementSyntaxNode>())
    {
        Emit(context, "continue;\n");
        return;
    }

    throw "unimplemented";

}

// Declaration References

static void EmitVal(EmitContext* context, RefPtr<Val> val)
{
    if (auto type = val.As<ExpressionType>())
    {
        EmitType(context, type);
    }
    else if (auto intVal = val.As<IntVal>())
    {
        Emit(context, intVal);
    }
    else
    {
        // Note(tfoley): ignore unhandled cases for semantics for now...
//		assert(!"unimplemented");
    }
}

static void EmitDeclRef(EmitContext* context, DeclRef declRef)
{
    // TODO: need to qualify a declaration name based on parent scopes/declarations

    // Emit the name for the declaration itself
    Emit(context, declRef.GetName());

    // If the declaration is nested directly in a generic, then
    // we need to output the generic arguments here
    auto parentDeclRef = declRef.GetParent();
    if (auto genericDeclRef = parentDeclRef.As<GenericDeclRef>())
    {
        Substitutions* subst = declRef.substitutions.Ptr();
        Emit(context, "<");
        int argCount = subst->args.Count();
        for (int aa = 0; aa < argCount; ++aa)
        {
            if (aa != 0) Emit(context, ",");
            EmitVal(context, subst->args[aa]);
        }
        Emit(context, ">");
    }

}

// Declarations

// Emit any modifiers that should go in front of a declaration
static void EmitModifiers(EmitContext* context, RefPtr<Decl> decl)
{
    for (auto mod = decl->modifiers.first; mod; mod = mod->next)
    {
        if (0) {}

        #define CASE(TYPE, KEYWORD) \
            else if(auto mod_##TYPE = mod.As<TYPE>()) Emit(context, #KEYWORD " ")

        CASE(RowMajorLayoutModifier, row_major);
        CASE(ColumnMajorLayoutModifier, column_major);
        CASE(HLSLNoInterpolationModifier, nointerpolation);
        CASE(HLSLPreciseModifier, precise);
        CASE(HLSLEffectSharedModifier, shared);
        CASE(HLSLGroupSharedModifier, groupshared);
        CASE(HLSLStaticModifier, static);
        CASE(HLSLUniformModifier, uniform);
        CASE(HLSLVolatileModifier, volatile);

        #undef CASE

        // TODO: eventually we should be checked these modifiers, but for
        // now we can emit them unchecked, I guess
        else if (auto uncheckedAttr = mod.As<HLSLUncheckedAttribute>())
        {
            Emit(context, "[");
            Emit(context, uncheckedAttr->nameToken.Content);
            auto& args = uncheckedAttr->args;
            auto argCount = args.Count();
            if (argCount != 0)
            {
                Emit(context, "(");
                for (int aa = 0; aa < argCount; ++aa)
                {
                    if (aa != 0) Emit(context, ", ");
                    EmitExpr(context, args[aa]);
                }
                Emit(context, ")");
            }
            Emit(context, "]");
        }

        else
        {
            // skip any extra modifiers
        }
    }
}


typedef unsigned int ESemanticMask;
enum
{
    kESemanticMask_None = 0,

    kESemanticMask_NoPackOffset = 1 << 0,

    kESemanticMask_Default = kESemanticMask_NoPackOffset,
};

static void EmitSemantic(EmitContext* context, RefPtr<HLSLSemantic> semantic, ESemanticMask mask)
{
    if (auto simple = semantic.As<HLSLSimpleSemantic>())
    {
        Emit(context, ": ");
        Emit(context, simple->name.Content);
    }
    else if(auto registerSemantic = semantic.As<HLSLRegisterSemantic>())
    {
        Emit(context, ": register(");
        Emit(context, registerSemantic->registerName.Content);
        if(registerSemantic->componentMask.Type != TokenType::Unknown)
        {
            Emit(context, ".");
            Emit(context, registerSemantic->componentMask.Content);
        }
        Emit(context, ")");
    }
    else if(auto packOffsetSemantic = semantic.As<HLSLPackOffsetSemantic>())
    {
        if(mask & kESemanticMask_NoPackOffset)
            return;

        Emit(context, ": packoffset(");
        Emit(context, packOffsetSemantic->registerName.Content);
        if(packOffsetSemantic->componentMask.Type != TokenType::Unknown)
        {
            Emit(context, ".");
            Emit(context, packOffsetSemantic->componentMask.Content);
        }
        Emit(context, ")");
    }
    else
    {
        assert(!"unimplemented");
    }
}


static void EmitSemantics(EmitContext* context, RefPtr<Decl> decl, ESemanticMask mask = kESemanticMask_Default )
{
    for (auto mod = decl->modifiers.first; mod; mod = mod->next)
    {
        auto semantic = mod.As<HLSLSemantic>();
        if (!semantic)
            continue;

        EmitSemantic(context, semantic, mask);
    }
}

static void EmitDeclsInContainer(EmitContext* context, RefPtr<ContainerDecl> container)
{
    for (auto member : container->Members)
    {
        EmitDecl(context, member);
    }
}

static void EmitTypeDefDecl(EmitContext* context, RefPtr<TypeDefDecl> decl)
{
    // TODO(tfoley): check if current compilation target even supports typedefs

    Emit(context, "typedef ");
    EmitType(context, decl->Type, decl->Name.Content);
    Emit(context, ";\n");
}

static void EmitStructDecl(EmitContext* context, RefPtr<StructSyntaxNode> decl)
{
    Emit(context, "struct ");
    Emit(context, decl->Name.Content);
    Emit(context, "\n{\n");

    // TODO(tfoley): Need to hoist members functions, etc. out to global scope
    EmitDeclsInContainer(context, decl);

    Emit(context, "};\n");
}

// Shared emit logic for variable declarations (used for parameters, locals, globals, fields)
static void EmitVarDeclCommon(EmitContext* context, VarDeclBaseRef declRef)
{
    EmitModifiers(context, declRef.GetDecl());

    EmitType(context, declRef.GetType(), declRef.GetName());

    EmitSemantics(context, declRef.GetDecl());

    // TODO(tfoley): technically have to apply substitution here too...
    if (auto initExpr = declRef.GetDecl()->Expr)
    {
        Emit(context, " = ");
        EmitExpr(context, initExpr);
    }
}

// Shared emit logic for variable declarations (used for parameters, locals, globals, fields)
static void EmitVarDeclCommon(EmitContext* context, RefPtr<VarDeclBase> decl)
{
    EmitVarDeclCommon(context, DeclRef(decl.Ptr(), nullptr).As<VarDeclBaseRef>());
}

static void EmitConstantBufferDecl(
    EmitContext*				context,
    RefPtr<VarDeclBase>			varDecl,
    RefPtr<ConstantBufferType>	cbufferType)
{
    // The data type that describes where stuff in the constant buffer should go
    RefPtr<ExpressionType> dataType = cbufferType->elementType;

    // We expect/require the data type to be a user-defined `struct` type
    if (auto declRefType = dataType->As<DeclRefType>())
    {
        Emit(context, "cbuffer ");
        Emit(context, declRefType->declRef.GetName());

        EmitSemantics(context, varDecl, kESemanticMask_None);

        Emit(context, "\n{\n");
        if (auto structRef = declRefType->declRef.As<StructDeclRef>())
        {
            for (auto field : structRef.GetMembersOfType<FieldDeclRef>())
            {
                EmitVarDeclCommon(context, field);
                Emit(context, ";\n");
            }
        }
        Emit(context, "}\n");
    }
    else
    {
        assert(!"unexpected");
    }
}

static void EmitVarDecl(EmitContext* context, RefPtr<VarDeclBase> decl)
{
    // As a special case, a variable using the `Constantbuffer<T>` type
    // should be translated into a `cbuffer` declaration if the target
    // requires it.
    //
    // TODO(tfoley): there might be a better way to detect this, e.g.,
    // with an attribute that gets attached to the variable declaration.
    if (auto cbufferType = decl->Type->As<ConstantBufferType>())
    {
        EmitConstantBufferDecl(context, decl, cbufferType);
        return;
    }

    EmitVarDeclCommon(context, decl);

    Emit(context, ";\n");
}

static void EmitParamDecl(EmitContext* context, RefPtr<ParameterSyntaxNode> decl)
{
    if (decl->HasModifier<InOutModifier>())
    {
        Emit(context, "inout ");
    }
    else if (decl->HasModifier<OutModifier>())
    {
        Emit(context, "out ");
    }


    EmitVarDeclCommon(context, decl);
}

static void EmitFuncDecl(EmitContext* context, RefPtr<FunctionSyntaxNode> decl)
{
    EmitModifiers(context, decl);

    // TODO: if a function returns an array type, or something similar that
    // isn't allowed by declarator syntax and/or language rules, we could
    // hypothetically wrap things in a `typedef` and work around it.

    EmitType(context, decl->ReturnType, decl->Name.Content);

    Emit(context, "(");
    bool first = true;
    for (auto paramDecl : decl->GetMembersOfType<ParameterSyntaxNode>())
    {
        if (!first) Emit(context, ", ");
        EmitParamDecl(context, paramDecl);
        first = false;
    }
    Emit(context, ")");

    EmitSemantics(context, decl);

    if (auto bodyStmt = decl->Body)
    {
        EmitBlockStmt(context, bodyStmt);
    }
    else
    {
        Emit(context, ";\n");
    }
}

static void EmitProgram(EmitContext* context, RefPtr<ProgramSyntaxNode> program)
{
    EmitDeclsInContainer(context, program);
}

static void EmitDecl(EmitContext* context, RefPtr<Decl> decl)
{
    // Don't emit code for declarations that came from the stdlib.
    //
    // TODO(tfoley): We probably need to relax this eventually,
    // since different targets might have different sets of builtins.
    if (decl->HasModifier<FromStdLibModifier>())
        return;

    if (auto typeDefDecl = decl.As<TypeDefDecl>())
    {
        EmitTypeDefDecl(context, typeDefDecl);
        return;
    }
    else if (auto structDecl = decl.As<StructSyntaxNode>())
    {
        EmitStructDecl(context, structDecl);
        return;
    }
    else if (auto varDecl = decl.As<VarDeclBase>())
    {
        EmitVarDecl(context, varDecl);
        return;
    }
    else if (auto funcDecl = decl.As<FunctionSyntaxNode>())
    {
        EmitFuncDecl(context, funcDecl);
        return;
    }
    else if (auto genericDecl = decl.As<GenericDecl>())
    {
        // Don't emit generic decls directly; we will only
        // ever emit particular instantiations of them.
        return;
    }
	else if (auto classDecl = decl.As<ClassSyntaxNode>())
	{
		return;
	}
    throw "unimplemented";
}

static void EmitDecl(EmitContext* context, RefPtr<DeclBase> declBase)
{
    if( auto decl = declBase.As<Decl>() )
    {
        EmitDecl(context, decl);
    }
    else if(auto declGroup = declBase.As<DeclGroup>())
    {
        for(auto d : declGroup->decls)
            EmitDecl(context, d);
    }
    else
    {
        throw "unimplemented";
    }
}

String EmitProgram(ProgramSyntaxNode* program)
{
    EmitContext context;

    // TODO(tfoley): only emit symbols on-demand, as needed by a particular entry point

    EmitProgram(&context, program);

    String code = context.sb.ProduceString();

    return code;

#if 0
    // HACK(tfoley): Invoke the D3D HLSL compiler on the result, to validate it

#ifdef _WIN32
    {
        HMODULE d3dCompiler = LoadLibraryA("d3dcompiler_47");
        assert(d3dCompiler);

        pD3DCompile D3DCompile_ = (pD3DCompile)GetProcAddress(d3dCompiler, "D3DCompile");
        assert(D3DCompile_);

        ID3DBlob* codeBlob;
        ID3DBlob* diagnosticsBlob;
        HRESULT hr = D3DCompile_(
            code.begin(),
            code.Length(),
            "spire",
            nullptr,
            nullptr,
            "main",
            "ps_5_0",
            0,
            0,
            &codeBlob,
            &diagnosticsBlob);
        if (codeBlob) codeBlob->Release();
        if (diagnosticsBlob)
        {
            String diagnostics = (char const*) diagnosticsBlob->GetBufferPointer();
            fprintf(stderr, "%s", diagnostics.begin());
            OutputDebugStringA(diagnostics.begin());
            diagnosticsBlob->Release();
        }
        if (FAILED(hr))
        {
            int f = 9;
        }
    }

    #include <d3dcompiler.h>
#endif
#endif

}


}} // Spire::Compiler