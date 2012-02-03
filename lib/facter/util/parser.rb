# This class acts as the factory and parent class for parsed
# facts such as scripts, text, json and yaml files.
#
# Parsers must subclass this class and provide their own #results method.
class Facter::Util::Parser
  attr_reader :filename

  class << self
    # Retrieve the set extension, if any
    attr_reader :extension
  end

  # Used by subclasses. Registers +ext+ as the extension to match.
  #
  # For support mutliple extensions you can pass an array of extensions as
  # +ext+.
  def self.matches_extension(ext)
    if ext.class == String then
      @extension = ext.downcase
    elsif ext.class == Array then
      @extension = ext.collect {|x| x.downcase }
    end
  end

  def self.file_extension(filename)
    File.extname(filename).sub(".", '')
  end

  def self.inherited(klass)
    @subclasses ||= []
    @subclasses << klass
  end

  def self.matches?(filename)
    raise "Must override the 'matches?' method for #{self}" unless extension

    [extension].flatten.to_a.include?(file_extension(filename).downcase)
  end

  def self.subclasses
    @subclasses ||= []
    @subclasses
  end

  def self.which_parser(filename)
    unless klass = subclasses.detect {|k| k.matches?(filename) }
      raise ArgumentError, "Could not find parser for #{filename}"
    end
    klass
  end

  def self.new(filename)
    klass = which_parser(filename)

    object = klass.allocate
    object.send(:initialize, filename)

    object
  end

  def initialize(filename)
    @filename = filename
  end

  # This method must be overwriten by subclasses to provide
  # the results (as a hash) of parsing the filename.
  def results
    raise "Must override the 'results' method for #{self}"
  end

  class YamlParser < self
    matches_extension "yaml"

    def results
      require 'yaml'

      YAML.load_file(filename)
    rescue Exception => e
      Facter.warn("Failed to handle #{filename} as yaml facts: #{e.class}: #{e}")
    end
  end

  class TextParser < self
    matches_extension "txt"

    def results
      result = {}
      File.readlines(filename).each do |line|

        if line.chomp =~ /^(.+)=(.+)$/
          result[$1] = $2
        end
      end
      result
    rescue Exception => e
      Facter.warn("Failed to handle #{filename} as text facts: #{e.class}: #{e}")
    end
  end

  class JsonParser < self
    matches_extension "json"

    def results
      begin
        require 'json'
      rescue LoadError
        require 'rubygems'
        retry
      end

      JSON.load(File.read(filename))
    end
  end

  class ScriptParser < self
    def self.matches?(file)
      File.executable?(file)
    end

    def results
      output = Facter::Util::Resolution.exec(filename)

      result = {}
      output.split("\n").each do |line|
        if line =~ /^(.+)=(.+)$/
          result[$1] = $2
        end
      end

      result
    rescue Exception => e
      Facter.warn("Failed to handle #{filename} as script facts: #{e.class}: #{e}")
      Facter.debug(e.backtrace.join("\n\t"))
    end
  end
end
