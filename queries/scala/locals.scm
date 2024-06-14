(template_body) @local.scope
(lambda_expression) @local.scope
(block) @local.scope

(function_declaration
      name: (identifier) @local.definition)

(function_definition
      name: (identifier) @local.definition) @local.scope

(parameter
  name: (identifier) @local.definition)

(class_parameter
  name: (identifier) @local.definition)

(binding
  name: (identifier) @local.definition)

(val_definition
  pattern: (identifier) @local.definition)

(var_definition
  pattern: (identifier) @local.definition)

(val_declaration
  name: (identifier) @local.definition)

(var_declaration
  name: (identifier) @local.definition)

(identifier) @local.reference

