use quote::ToTokens;
use syn::{visit_mut::VisitMut, Attribute, Expr};

/// Expands parameter definitions to values that are read from standard arguments automatically
pub struct ParameterExpander {}

impl VisitMut for ParameterExpander {
    fn visit_item_const_mut(&mut self, i: &mut syn::ItemConst) {
        if let Some(ParameterExpression { name, default }) = get_parameter(&i) {
            i.expr = syn::parse_quote! {
                LazyCell::new(|| {
                    std::env::args()
                        .skip(1)
                        .collect::<Vec<_>>()
                        .chunks_exact(2)
                        .find(|item| item[0] == #name)
                        .map(|item| item[1].parse().unwrap())
                        .unwrap_or_else(#default)
                })
            };
        }
    }
}

struct ParameterExpression {
    name: String,
    default: Expr,
}

/// Extracts a parameter definition from a const item
fn get_parameter(i: &syn::ItemConst) -> Option<ParameterExpression> {
    // extract the #[doc] attribute
    if let Some(doc) = get_doc_content(&i.attrs) {
        // make sure it makes reference to a parameter
        if doc.trim() == "RealParam" {
            // make sure the item is a LazyCell
            if let Some(default) = get_lazy_cell_initializer(&i.expr) {
                return Some(ParameterExpression {
                    name: i.ident.to_token_stream().to_string(),
                    // doc: doc.trim().to_string(),
                    default,
                });
            }
        }
    }
    None
}

/// Finds and extract the content of a #[doc] attribute
///
/// ```#[doc = "Hello"] → Some("Hello")```
fn get_doc_content(attrs: &Vec<Attribute>) -> Option<String> {
    for attr in attrs {
        if attr.path().is_ident("doc") {
            if let syn::Meta::NameValue(ref meta) = attr.meta {
                if let syn::Expr::Lit(syn::ExprLit { lit, attrs: _ }) = &meta.value {
                    if let syn::Lit::Str(ref content) = lit {
                        return Some(content.value().to_string());
                    }
                }
            }
        }
    }
    None
}

/// Checks if the expression is a call to create a new LazyCell and returns the initializer expression
///
/// ```LazyCell::new(|| 42.0) → Some(|| 42.0)```
fn get_lazy_cell_initializer(expr: &Expr) -> Option<Expr> {
    if let Expr::Call(expr_call) = expr {
        if let Expr::Path(expr_path) = expr_call.func.as_ref() {
            if path_is_lazy_cell_new(&expr_path.path) {
                return Some(expr_call.args[0].clone());
            }
        }
    }
    None
}

fn path_is_lazy_cell_new(path: &syn::Path) -> bool {
    // TODO: accept other variants
    path.segments.len() == 2
        && path.segments[0].ident == "LazyCell"
        && path.segments[1].ident == "new"
}
