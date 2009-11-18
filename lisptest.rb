#require "lispu.rb"
def x
 puts "hello"
end
x = 30
lblock
 (x)
 (puts 30)
end
puts x

