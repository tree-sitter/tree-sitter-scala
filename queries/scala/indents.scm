[
  (ERROR ":")
  (template_body)
  (indented_block)
  (block)
  (parameters)
  (arguments)
  (match_expression)
] @indent.begin 


(arguments ")" @indent.end)

"}" @indent.end

[
  ")"
  "]"
  "}"
] @indent.branch

