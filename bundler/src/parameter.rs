#[derive(Debug)]
pub struct Parameter {
    name: String,
    default: Option<f32>,
    min: Option<f32>,
    max: Option<f32>,
}
