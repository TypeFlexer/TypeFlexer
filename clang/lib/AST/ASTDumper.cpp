//===--- ASTDumper.cpp - Dumping implementation for ASTs ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the AST dump methods, which dump out the
// AST in a form that exposes type details and other fields.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTDumper.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclLookups.h"
#include "clang/AST/JSONNodeDumper.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/Support/raw_ostream.h"
using namespace clang;
using namespace clang::comments;

void ASTDumper::dumpLookups(const DeclContext *DC, bool DumpDecls) {
  NodeDumper.AddChild([=] {
    OS << "StoredDeclsMap ";
    NodeDumper.dumpBareDeclRef(cast<Decl>(DC));

    const DeclContext *Primary = DC->getPrimaryContext();
    if (Primary != DC) {
      OS << " primary";
      NodeDumper.dumpPointer(cast<Decl>(Primary));
    }

    bool HasUndeserializedLookups = Primary->hasExternalVisibleStorage();

    auto Range = getDeserialize()
                     ? Primary->lookups()
                     : Primary->noload_lookups(/*PreserveInternalState=*/true);
    for (auto I = Range.begin(), E = Range.end(); I != E; ++I) {
      DeclarationName Name = I.getLookupName();
      DeclContextLookupResult R = *I;

      NodeDumper.AddChild([=] {
        OS << "DeclarationName ";
        {
          ColorScope Color(OS, ShowColors, DeclNameColor);
          OS << '\'' << Name << '\'';
        }

        for (DeclContextLookupResult::iterator RI = R.begin(), RE = R.end();
             RI != RE; ++RI) {
          NodeDumper.AddChild([=] {
            NodeDumper.dumpBareDeclRef(*RI);

            if (!(*RI)->isUnconditionallyVisible())
              OS << " hidden";

            // If requested, dump the redecl chain for this lookup.
            if (DumpDecls) {
              // Dump earliest decl first.
              std::function<void(Decl *)> DumpWithPrev = [&](Decl *D) {
                if (Decl *Prev = D->getPreviousDecl())
                  DumpWithPrev(Prev);
                Visit(D);
              };
              DumpWithPrev(*RI);
            }
          });
        }
      });
    }

    if (HasUndeserializedLookups) {
      NodeDumper.AddChild([=] {
        ColorScope Color(OS, ShowColors, UndeserializedColor);
        OS << "<undeserialized lookups>";
      });
    }
  });
}

template <typename SpecializationDecl>
void ASTDumper::dumpTemplateDeclSpecialization(const SpecializationDecl *D,
                                               bool DumpExplicitInst,
                                               bool DumpRefOnly) {
  bool DumpedAny = false;
  for (const auto *RedeclWithBadType : D->redecls()) {
    // FIXME: The redecls() range sometimes has elements of a less-specific
    // type. (In particular, ClassTemplateSpecializationDecl::redecls() gives
    // us TagDecls, and should give CXXRecordDecls).
    auto *Redecl = dyn_cast<SpecializationDecl>(RedeclWithBadType);
    if (!Redecl) {
      // Found the injected-class-name for a class template. This will be dumped
      // as part of its surrounding class so we don't need to dump it here.
      assert(isa<CXXRecordDecl>(RedeclWithBadType) &&
             "expected an injected-class-name");
      continue;
    }

    switch (Redecl->getTemplateSpecializationKind()) {
    case TSK_ExplicitInstantiationDeclaration:
    case TSK_ExplicitInstantiationDefinition:
      if (!DumpExplicitInst)
        break;
      LLVM_FALLTHROUGH;
    case TSK_Undeclared:
    case TSK_ImplicitInstantiation:
      if (DumpRefOnly)
        NodeDumper.dumpDeclRef(Redecl);
      else
        Visit(Redecl);
      DumpedAny = true;
      break;
    case TSK_ExplicitSpecialization:
      break;
    }
  }

  // Ensure we dump at least one decl for each specialization.
  if (!DumpedAny)
    NodeDumper.dumpDeclRef(D);
}

template <typename TemplateDecl>
void ASTDumper::dumpTemplateDecl(const TemplateDecl *D, bool DumpExplicitInst) {
  dumpTemplateParameters(D->getTemplateParameters());

  Visit(D->getTemplatedDecl());

  if (GetTraversalKind() == TK_AsIs) {
    for (const auto *Child : D->specializations())
      dumpTemplateDeclSpecialization(Child, DumpExplicitInst,
                                     !D->isCanonicalDecl());
  }
}

void ASTDumper::VisitFunctionTemplateDecl(const FunctionTemplateDecl *D) {
  // FIXME: We don't add a declaration of a function template specialization
  // to its context when it's explicitly instantiated, so dump explicit
  // instantiations when we dump the template itself.
  dumpTemplateDecl(D, true);
}

void ASTDumper::VisitClassTemplateDecl(const ClassTemplateDecl *D) {
  dumpTemplateDecl(D, false);
}

void ASTDumper::VisitVarTemplateDecl(const VarTemplateDecl *D) {
  dumpTemplateDecl(D, false);
}

//===----------------------------------------------------------------------===//
// Type method implementations
//===----------------------------------------------------------------------===//

void QualType::dump(const char *msg) const {
  if (msg)
    llvm::errs() << msg << ": ";
  dump();
}

LLVM_DUMP_METHOD void QualType::dump() const {
  ASTDumper Dumper(llvm::errs(), /*ShowColors=*/false);
  Dumper.Visit(*this);
}

LLVM_DUMP_METHOD void QualType::dump(llvm::raw_ostream &OS,
                                     const ASTContext &Context) const {
  ASTDumper Dumper(OS, Context, Context.getDiagnostics().getShowColors());
  Dumper.Visit(*this);
}

LLVM_DUMP_METHOD void Type::dump() const { QualType(this, 0).dump(); }

LLVM_DUMP_METHOD void Type::dump(llvm::raw_ostream &OS,
                                 const ASTContext &Context) const {
  QualType(this, 0).dump(OS, Context);
}

//===----------------------------------------------------------------------===//
// Decl method implementations
//===----------------------------------------------------------------------===//

LLVM_DUMP_METHOD void Decl::dump() const { dump(llvm::errs()); }

LLVM_DUMP_METHOD void Decl::dump(raw_ostream &OS, bool Deserialize,
                                 ASTDumpOutputFormat Format) const {
  ASTContext &Ctx = getASTContext();
  const SourceManager &SM = Ctx.getSourceManager();

  if (ADOF_JSON == Format) {
    JSONDumper P(OS, SM, Ctx, Ctx.getPrintingPolicy(),
                 &Ctx.getCommentCommandTraits());
    (void)Deserialize; // FIXME?
    P.Visit(this);
  } else {
    ASTDumper P(OS, Ctx, Ctx.getDiagnostics().getShowColors());
    P.setDeserialize(Deserialize);
    P.Visit(this);
  }
}

LLVM_DUMP_METHOD void Decl::dumpColor() const {
  const ASTContext &Ctx = getASTContext();
  ASTDumper P(llvm::errs(), Ctx, /*ShowColors=*/true);
  P.Visit(this);
}

LLVM_DUMP_METHOD void DeclContext::dumpLookups() const {
  dumpLookups(llvm::errs());
}

LLVM_DUMP_METHOD void DeclContext::dumpLookups(raw_ostream &OS,
                                               bool DumpDecls,
                                               bool Deserialize) const {
  const DeclContext *DC = this;
  while (!DC->isTranslationUnit())
    DC = DC->getParent();
  const ASTContext &Ctx = cast<TranslationUnitDecl>(DC)->getASTContext();
  ASTDumper P(OS, Ctx, Ctx.getDiagnostics().getShowColors());
  P.setDeserialize(Deserialize);
  P.dumpLookups(this, DumpDecls);
}

//===----------------------------------------------------------------------===//
// Stmt method implementations
//===----------------------------------------------------------------------===//

LLVM_DUMP_METHOD void Stmt::dump() const {
  ASTDumper P(llvm::errs(), /*ShowColors=*/false);
  P.Visit(this);
}

LLVM_DUMP_METHOD void Stmt::dump(raw_ostream &OS,
                                 const ASTContext &Context) const {
  ASTDumper P(OS, Context, Context.getDiagnostics().getShowColors());
  P.Visit(this);
}

LLVM_DUMP_METHOD void Stmt::dumpColor() const {
  ASTDumper P(llvm::errs(), /*ShowColors=*/true);
  P.Visit(this);
}

//===----------------------------------------------------------------------===//
// Comment method implementations
//===----------------------------------------------------------------------===//

LLVM_DUMP_METHOD void Comment::dump() const {
  const auto *FC = dyn_cast<FullComment>(this);
  if (!FC)
    return;
  ASTDumper Dumper(llvm::errs(), /*ShowColors=*/false);
  Dumper.Visit(FC, FC);
}

LLVM_DUMP_METHOD void Comment::dump(raw_ostream &OS,
                                    const ASTContext &Context) const {
  const auto *FC = dyn_cast<FullComment>(this);
  if (!FC)
    return;
  ASTDumper Dumper(OS, Context, Context.getDiagnostics().getShowColors());
  Dumper.Visit(FC, FC);
}

LLVM_DUMP_METHOD void Comment::dumpColor() const {
  const auto *FC = dyn_cast<FullComment>(this);
  if (!FC)
    return;
  ASTDumper Dumper(llvm::errs(), /*ShowColors=*/true);
  Dumper.Visit(FC, FC);
}

//===----------------------------------------------------------------------===//
// APValue method implementations
//===----------------------------------------------------------------------===//

LLVM_DUMP_METHOD void APValue::dump() const {
  ASTDumper Dumper(llvm::errs(), /*ShowColors=*/false);
  Dumper.Visit(*this, /*Ty=*/QualType());
}

LLVM_DUMP_METHOD void APValue::dump(raw_ostream &OS,
                                    const ASTContext &Context) const {
  ASTDumper Dumper(llvm::errs(), Context,
                   Context.getDiagnostics().getShowColors());
  Dumper.Visit(*this, /*Ty=*/Context.getPointerType(Context.CharTy));
}

// Checked C specific methods.
void ASTDumper::VisitCastExpr(const CastExpr *Node) {
  if (const BoundsExpr *NormalizedBounds = Node->getNormalizedBoundsExpr())
    NodeDumper.AddChild([=] {
      OS << "Normalized Bounds";
      Visit(NormalizedBounds);
    });

  if (const BoundsExpr *SubExprBounds = Node->getSubExprBoundsExpr())
    NodeDumper.AddChild([=] {
      OS << "Inferred SubExpr Bounds";
      Visit(SubExprBounds);
    });
}

void ASTDumper::VisitDeclRefExpr(const DeclRefExpr *Node) {
  if (Node->GetTypeArgumentInfo() &&
      !Node->GetTypeArgumentInfo()->typeArgumentss().empty()) {
    for (const auto& tn : Node->GetTypeArgumentInfo()->typeArgumentss()) {
      Visit(tn.typeName);
    }
  }
}

void ASTDumper::VisitArraySubscriptExpr(const ArraySubscriptExpr *Node) {
  if (const BoundsExpr *Bounds = Node->getBoundsExpr()) {
    NodeDumper.AddChild([=] {
      OS << "Bounds ";
      NodeDumper.Visit(Node->getBoundsCheckKind());
      Visit(Bounds);
    });
  }
}

void ASTDumper::VisitMemberExpr(const MemberExpr *Node) {
  if (const BoundsExpr *Bounds = Node->getBoundsExpr()) {
    NodeDumper.AddChild([=] {
      OS << "Base Expr Bounds";
      Visit(Bounds);
    });
  }
}

void ASTDumper::VisitUnaryOperator(const UnaryOperator *Node) {
  if (const BoundsExpr *Bounds = Node->getBoundsExpr()) {
    NodeDumper.AddChild([=] {
      OS << "Bounds ";
      NodeDumper.Visit(Node->getBoundsCheckKind());
      Visit(Bounds);
    });
  }
}

void ASTDumper::VisitCompoundStmt(const CompoundStmt *Node) {
  VisitStmt(Node);
  CheckedScopeSpecifier WrittenCSS = Node->getWrittenCheckedSpecifier();
  TaintedScopeSpecifier WrittenTaintedSS = Node->getWrittenTaintedSpecifier();
  MirrorScopeSpecifier WrittenMirrorSS = Node->getWrittenMirrorSpecifier();
  TLIBScopeSpecifier WrittenTLIBSS = Node->getWrittenTLIBSpecifier();
  switch (WrittenCSS) {
    case CSS_None: break;
    case CSS_Unchecked: OS << " _Unchecked "; break;
    case CSS_Bounds: OS <<  " _Checked _Bounds_only "; break;
    case CSS_Memory: OS << " _Checked "; break;
  }
  switch (WrittenTaintedSS) {
  case Tainted_None: break;
  case Tainted_UnTainted: break;
  case Tainted_Bounds: OS <<  " _Tainted _Bounds_only "; break;
  case Tainted_Memory: OS << " _Tainted "; break;
  }
  //Repeat the above for Mirror and TLIB too

  CheckedScopeSpecifier CSS = Node->getCheckedSpecifier();
  if (CSS != CSS_Unchecked) {
     OS << "checking-state ";
     OS << (CSS == CSS_Bounds ? "bounds" :"bounds-and-types");
  }
}

void ASTDumper::VisitRangeBoundsExpr(const RangeBoundsExpr *Node) {
  if (Node->getKind() != BoundsExpr::Kind::Range)
    NodeDumper.Visit(Node->getKind());
  if (Node->hasRelativeBoundsClause()) {
    RelativeBoundsClause *Expr =
        cast<RelativeBoundsClause>(Node->getRelativeBoundsClause());
    OS << " rel_align : ";
    if (Expr->getClauseKind() == RelativeBoundsClause::Kind::Type) {
      QualType Ty = cast<RelativeTypeBoundsClause>(Expr)->getType();
      NodeDumper.dumpType(Ty);
    } else if (Expr->getClauseKind() == RelativeTypeBoundsClause::Kind::Const) {
      Visit(cast<RelativeConstExprBoundsClause>(Expr)->getConstExpr());
    } else {
      llvm_unreachable("unexpected kind field of relative bounds clause");
    }
  }
}

void ASTDumper::VisitInteropTypeExpr(const InteropTypeExpr *Node) {
  Visit(Node->getType());
}

void ASTDumper::VisitFunctionProtoType(const FunctionProtoType *T) {
  VisitFunctionType(T);

  unsigned numParams = T->getNumParams();
  for (unsigned i = 0; i < numParams; i++) {
    QualType PT = T->getParamType(i);
    Visit(PT);

    const BoundsAnnotations Annots = T->getParamAnnots(i);
    if (const BoundsExpr *Bounds = Annots.getBoundsExpr())
      NodeDumper.AddChild([=] {
        OS << "Bounds";
        Visit(Bounds);
      });
    if (const InteropTypeExpr *IT = Annots.getInteropTypeExpr())
      NodeDumper.AddChild([=] {
        OS << "InteropType";
        Visit(IT);
      });
  }

  if (T->getExtProtoInfo().Variadic)
    NodeDumper.AddChild([=] { OS << "..."; });

  BoundsAnnotations ReturnAnnots = T->getReturnAnnots();
  if (const BoundsExpr *Bounds = ReturnAnnots.getBoundsExpr())
    NodeDumper.AddChild([=] {
      OS << "Return bounds";
      Visit(Bounds);
    });
  if (const InteropTypeExpr *IT = ReturnAnnots.getInteropTypeExpr())
    NodeDumper.AddChild([=] {
      OS << "Return interopType";
      Visit(IT);
    });
}
