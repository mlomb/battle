use crate::read_file;
use std::path::Path;
use syn::visit_mut::VisitMut;

pub struct Expander<'a> {
    pub base_path: &'a Path,
    pub crate_name: &'a str,
}

impl Expander<'_> {
    fn expand_items(&self, items: &mut Vec<syn::Item>) {
        let mut new_items = vec![];
        for item in items.drain(..) {
            self.expand_item(item, &mut new_items);
        }
        *items = new_items;
    }

    fn expand_item(&self, item: syn::Item, new_items: &mut Vec<syn::Item>) {
        match item {
            syn::Item::Use(ref item) => {
                if let syn::UseTree::Path(ref path) = item.tree {
                    if path.ident == self.crate_name {
                        let mut innner = item.clone();
                        innner.tree = path.tree.as_ref().clone();
                        new_items.push(innner.try_into().unwrap());
                        return;
                    }
                }
            }
            syn::Item::ExternCrate(ref item) => {
                if item.ident == self.crate_name {
                    /*eprintln!(
                        "expanding crate {} in {}",
                        self.crate_name,
                        self.base_path.to_str().unwrap()
                    );*/
                    let code =
                        read_file(&self.base_path.join("lib.rs")).expect("failed to read lib.rs");
                    let lib = syn::parse_file(&code).expect("failed to parse lib.rs");
                    new_items.extend(lib.items);
                    return;
                }
            }
            _ => {}
        }

        new_items.push(item);
    }

    fn expand_mods(&self, item: &mut syn::ItemMod) {
        if item.content.is_some() {
            return;
        }
        let name = item.ident.to_string();
        let other_base_path = self.base_path.join(&name);
        let (base_path, code) = vec![
            (self.base_path, format!("{}.rs", name)),
            (&other_base_path, String::from("mod.rs")),
        ]
        .into_iter()
        .flat_map(|(base_path, file_name)| {
            read_file(&base_path.join(file_name)).map(|code| (base_path, code))
        })
        .next()
        .expect("mod not found");
        //eprintln!("expanding mod {} in {}", name, base_path.to_str().unwrap());

        if let Ok(mut file) = syn::parse_file(&code) {
            Expander {
                base_path,
                crate_name: self.crate_name,
            }
            .visit_file_mut(&mut file);
            item.content = Some((Default::default(), file.items));
        }
    }
}

impl VisitMut for Expander<'_> {
    fn visit_file_mut(&mut self, file: &mut syn::File) {
        for it in &mut file.attrs {
            self.visit_attribute_mut(it)
        }
        self.expand_items(&mut file.items);
        for it in &mut file.items {
            self.visit_item_mut(it)
        }
    }

    fn visit_item_mod_mut(&mut self, item: &mut syn::ItemMod) {
        for it in &mut item.attrs {
            self.visit_attribute_mut(it)
        }
        self.visit_visibility_mut(&mut item.vis);
        self.visit_ident_mut(&mut item.ident);
        self.expand_mods(item);
        if let Some(ref mut it) = item.content {
            for it in &mut (it).1 {
                self.visit_item_mut(it);
            }
        }
    }
}
