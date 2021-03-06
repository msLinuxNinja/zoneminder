
package WSNotification::Elements::TopicNamespace;
use strict;
use warnings;

{ # BLOCK to scope variables

sub get_xmlns { 'http://docs.oasis-open.org/wsn/t-1' }

__PACKAGE__->__set_name('TopicNamespace');
__PACKAGE__->__set_nillable();
__PACKAGE__->__set_minOccurs();
__PACKAGE__->__set_maxOccurs();
__PACKAGE__->__set_ref();
use base qw(
    SOAP::WSDL::XSD::Typelib::Element
    WSNotification::Types::TopicNamespaceType
);

}

1;


=pod

=head1 NAME

WSNotification::Elements::TopicNamespace

=head1 DESCRIPTION

Perl data type class for the XML Schema defined element
TopicNamespace from the namespace http://docs.oasis-open.org/wsn/t-1.







=head1 METHODS

=head2 new

 my $element = WSNotification::Elements::TopicNamespace->new($data);

Constructor. The following data structure may be passed to new():

 { # WSNotification::Types::TopicNamespaceType
   Topic =>  {
   },
 },

=head1 AUTHOR

Generated by SOAP::WSDL

=cut

